"""D2R Rooms relay, self-hosted variant: one Python process, all rooms in memory.

Speaks relay/PROTOCOL.md v1, the same as the Cloudflare Worker, so a client
cannot tell them apart. Put it behind a TLS proxy (docker-compose.yml has
Caddy) and give players the wss:// address.

Settings come from the environment (all optional):
  RELAY_NAME   shown in /healthz                      ("D2R Rooms relay")
  RELAY_KEYS   comma-separated keys a host must send  (empty = open to all)
  RELAY_HOST   address to listen on                   (0.0.0.0)
  RELAY_PORT   port to listen on                      (8080)
  MAX_ROOMS    rooms with a host online at once       (500)
  NEAR_LIMIT   share of MAX_ROOMS that stops new rooms (0.8)
  RELAY_OPEN   0 = status "limit": no new rooms or pipes (maintenance) (1)
"""

import asyncio
import hashlib
import hmac
import json
import os
import re
import time
from dataclasses import dataclass, field
from typing import Optional

from websockets.asyncio.server import ServerConnection, serve
from websockets.datastructures import Headers
from websockets.exceptions import ConnectionClosed
from websockets.http11 import Request, Response

VERSION = 1
PIPES = 7
MAX_MESSAGE = 65536
RATE = 256 * 1024  # bytes a second per pipe and direction
BURST = 1024 * 1024
CLAIM_KEPT = 24 * 3600

OPEN, DATA, CLOSE = 1, 2, 3
PING, PONG = '{"t":"ping"}', '{"t":"pong"}'

ROOM_PATH = re.compile(r"^/v1/rooms/([A-Za-z0-9_-]{16,64})/(host|join)$")
TOKEN = re.compile(r"^[A-Za-z0-9_-]{16,128}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")


def sha256(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


@dataclass
class Settings:
    name: str = "D2R Rooms relay"
    keys: tuple[str, ...] = ()
    host: str = "0.0.0.0"
    port: int = 8080
    max_rooms: int = 500
    near_limit: float = 0.8
    open: bool = True
    rate: int = RATE
    burst: int = BURST

    @staticmethod
    def from_env() -> "Settings":
        e = os.environ
        return Settings(
            name=e.get("RELAY_NAME", "D2R Rooms relay"),
            keys=tuple(k.strip() for k in e.get("RELAY_KEYS", "").split(",") if k.strip()),
            host=e.get("RELAY_HOST", "0.0.0.0"),
            port=int(e.get("RELAY_PORT", "8080")),
            max_rooms=int(e.get("MAX_ROOMS", "500")),
            near_limit=float(e.get("NEAR_LIMIT", "0.8")),
            open=e.get("RELAY_OPEN", "1") != "0",
        )


class Bucket:
    """Bytes allowed per second with a burst; `take` says whether it stayed inside."""

    def __init__(self, rate: int, burst: int) -> None:
        self.rate, self.burst = float(rate), float(burst)
        self.tokens, self.stamp = float(burst), time.monotonic()

    def take(self, n: int) -> bool:
        t = time.monotonic()
        self.tokens = min(self.burst, self.tokens + (t - self.stamp) * self.rate)
        self.stamp = t
        self.tokens -= n
        return self.tokens >= 0


@dataclass(eq=False)
class Pipe:
    n: int
    ws: ServerConnection
    up: Bucket  # joiner -> host
    down: Bucket  # host -> joiner


@dataclass(eq=False)
class Room:
    room_id: str
    secret_hash: str
    join_hash: str
    host: Optional[ServerConnection] = None
    pipes: dict[int, Pipe] = field(default_factory=dict)
    released_at: float = 0.0

    def free_pipe(self) -> Optional[int]:
        if len(self.pipes) >= PIPES:
            return None
        return next(n for n in range(1, 256) if n not in self.pipes)


def _headers(content_type: str, length: int) -> Headers:
    return Headers([("Content-Type", content_type), ("Content-Length", str(length)), ("Cache-Control", "no-store")])


def refusal(status: int, error: str) -> Response:
    body = json.dumps({"error": error}).encode()
    return Response(status, "", _headers("application/json", len(body)), body)


class Relay:
    def __init__(self, settings: Settings) -> None:
        self.s = settings
        self.rooms: dict[str, Room] = {}

    # --- before the upgrade ----------------------------------------------

    def online(self) -> int:
        return sum(1 for r in self.rooms.values() if r.host is not None)

    def usage(self) -> float:
        return self.online() / max(1, self.s.max_rooms)

    def status(self) -> str:
        """PROTOCOL.md §1.1. On a VPS the cap is the room count; there is no money budget."""
        if not self.s.open:
            return "limit"
        return "near_limit" if self.usage() >= self.s.near_limit else "active"

    def health(self) -> dict:
        status = self.status()
        return {"ok": True, "relay": VERSION, "name": self.s.name, "status": status, "open": status != "limit",
                "usage": round(self.usage(), 3), "pipes": PIPES, "max_message": MAX_MESSAGE}

    def check(self, connection: ServerConnection, request: Request) -> Optional[Response]:
        path = request.path.split("?", 1)[0].rstrip("/")
        if path == "/healthz":
            body = json.dumps(self.health()).encode()
            return Response(200, "OK", _headers("application/json", len(body)), body)
        m = ROOM_PATH.match(path)
        if not m:
            return refusal(404, "not_found")
        if "websocket" not in request.headers.get("Upgrade", "").lower():
            return refusal(426, "upgrade_required")
        room_id, side = m.group(1), m.group(2)
        h = request.headers
        self.sweep()
        if side == "host":
            if self.s.keys and not any(hmac.compare_digest(h.get("X-Relay-Key", ""), k) for k in self.s.keys):
                return refusal(401, "relay_key")
            secret, join_hash = h.get("X-Host-Secret", ""), h.get("X-Join-Key-Hash", "")
            if not TOKEN.match(secret) or not HEX64.match(join_hash):
                return refusal(400, "bad_request")
            room = self.rooms.get(room_id)
            if room is not None and not hmac.compare_digest(room.secret_hash, sha256(secret)):
                return refusal(403, "bad_secret")
            if room is None and self.status() != "active":
                return refusal(503, "near_limit")
            connection.relay = ("host", room_id, sha256(secret), join_hash)
            return None
        key = h.get("X-Join-Key", "")
        if not TOKEN.match(key):
            return refusal(400, "bad_request")
        room = self.rooms.get(room_id)
        if room is None or room.host is None:
            return refusal(404, "no_room")
        if not hmac.compare_digest(room.join_hash, sha256(key)):
            return refusal(403, "bad_join_key")
        if self.status() == "limit":
            return refusal(503, "over_budget")
        if room.free_pipe() is None:
            return refusal(409, "room_full")
        connection.relay = ("join", room_id, "", "")
        return None

    def sweep(self) -> None:
        t = time.monotonic()
        for room_id, room in list(self.rooms.items()):
            if room.host is None and t - room.released_at > CLAIM_KEPT:
                del self.rooms[room_id]

    # --- sockets -----------------------------------------------------------

    async def handle(self, ws: ServerConnection) -> None:
        side, room_id, secret_hash, join_hash = ws.relay
        if side == "host":
            await self.host(ws, room_id, secret_hash, join_hash)
        else:
            await self.join(ws, room_id)

    async def host(self, ws: ServerConnection, room_id: str, secret_hash: str, join_hash: str) -> None:
        room = self.rooms.get(room_id)
        if room is None:
            room = self.rooms[room_id] = Room(room_id, secret_hash, join_hash)
        old = room.host
        room.host, room.join_hash = ws, join_hash
        if old is not None:
            await self.drop_pipes(room, 1001, "host_gone")
            await old.close(4000, "replaced")
        await ws.send(json.dumps({"t": "hello", "v": VERSION, "pipes": PIPES, "max_message": MAX_MESSAGE}, separators=(",", ":")))
        try:
            async for msg in ws:
                if isinstance(msg, str):
                    if msg == PING:
                        await ws.send(PONG)
                    continue
                if len(msg) < 2:
                    continue
                kind, n = msg[0], msg[1]
                pipe = room.pipes.get(n)
                if pipe is None:
                    continue
                if kind == DATA:
                    if not pipe.down.take(len(msg) - 2):
                        await self.close_pipe(room, pipe, "rate", tell_host=True)
                        continue
                    try:
                        await pipe.ws.send(msg[2:])
                    except ConnectionClosed:
                        pass
                elif kind == CLOSE:
                    await self.close_pipe(room, pipe, msg[2:].decode("utf-8", "replace"), tell_host=False)
        except ConnectionClosed:
            pass
        finally:
            if room.host is ws:
                room.host = None
                room.released_at = time.monotonic()
                await self.drop_pipes(room, 1001, "host_gone")

    async def join(self, ws: ServerConnection, room_id: str) -> None:
        room = self.rooms.get(room_id)
        n = room.free_pipe() if room is not None and room.host is not None else None
        if n is None:  # the room changed between the check and the upgrade
            await ws.close(1013, "room_full")
            return
        pipe = room.pipes[n] = Pipe(n, ws, Bucket(self.s.rate, self.s.burst), Bucket(self.s.rate, self.s.burst))
        host = room.host
        try:
            await host.send(bytes([OPEN, n]))
            await ws.send(json.dumps({"t": "hello", "v": VERSION, "pipe": n, "pipes": PIPES, "max_message": MAX_MESSAGE}, separators=(",", ":")))
            async for msg in ws:
                if isinstance(msg, str):
                    if msg == PING:
                        await ws.send(PONG)
                    continue
                if len(msg) > MAX_MESSAGE:
                    await ws.close(1009, "too big")
                    break
                if not pipe.up.take(len(msg)):
                    await self.close_pipe(room, pipe, "rate", tell_host=True)
                    break
                if room.pipes.get(n) is not pipe or room.host is not host:
                    break
                try:
                    await host.send(bytes([DATA, n]) + msg)
                except ConnectionClosed:
                    break
        except ConnectionClosed:
            pass
        finally:
            if room.pipes.get(n) is pipe:
                del room.pipes[n]
                if room.host is host:
                    try:
                        await host.send(bytes([CLOSE, n]))
                    except ConnectionClosed:
                        pass

    async def close_pipe(self, room: Room, pipe: Pipe, reason: str, tell_host: bool) -> None:
        if room.pipes.get(pipe.n) is not pipe:
            return
        del room.pipes[pipe.n]
        if tell_host and room.host is not None:
            try:
                await room.host.send(bytes([CLOSE, pipe.n]) + reason.encode())
            except ConnectionClosed:
                pass
        await pipe.ws.close(1000, reason[:120])

    async def drop_pipes(self, room: Room, code: int, reason: str) -> None:
        pipes, room.pipes = list(room.pipes.values()), {}
        for p in pipes:
            await p.ws.close(code, reason)


async def run(settings: Settings, ready: Optional[asyncio.Future] = None) -> None:
    """Serves until cancelled; `ready` gets the Relay once the port listens."""
    relay = Relay(settings)
    async with serve(
        relay.handle,
        settings.host,
        settings.port,
        process_request=relay.check,
        max_size=MAX_MESSAGE + 2,
        compression=None,  # game bytes are small and latency matters
        server_header=None,
    ) as server:
        if ready is not None:
            ready.set_result(relay)
        await server.serve_forever()


if __name__ == "__main__":
    s = Settings.from_env()
    print(f"{s.name}: relay v{VERSION} on {s.host}:{s.port}, {'keys required' if s.keys else 'open to all hosts'}", flush=True)
    asyncio.run(run(s))
