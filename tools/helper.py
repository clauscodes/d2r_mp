"""Reference helper: the client side of the protocol in plain Python.

It does what the launcher's Multiplayer tab will do, so the server can be tried
with real games before the C++ helper exists:

    python helper.py list --server https://rooms.example.net
    python helper.py host --server URL --name "Baal runs" --mod d2r-vr --version 0.175.0
    python helper.py join --server URL --code K7QX --mod d2r-pcvr

Host: D2R hosts a TCP/IP game as usual; every joiner's connection is carried to
the local game at --game (default 127.0.0.1:4000).
Join: the helper listens on --listen (default 127.0.0.1:4000); in D2R join the
TCP/IP game 127.0.0.1. Each TCP connection the game opens gets its own tunnel.
"""

import argparse
import asyncio
import contextlib
import json
import sys
from pathlib import Path
from typing import Optional

import httpx
from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed, InvalidStatus

CHUNK = 16 * 1024
STATE_EVERY = 5.0
CLOSE_REPLACED = 4000  # the server's code when a second control socket took over


def ws_base(server: str) -> str:
    server = server.rstrip("/")
    if server.startswith("https://"):
        return "wss://" + server[len("https://"):]
    if server.startswith("http://"):
        return "ws://" + server[len("http://"):]
    return server


def split_addr(addr: str, default_port: int = 4000) -> tuple[str, int]:
    host, _, port = addr.rpartition(":")
    if not host:
        return addr, default_port
    return host, int(port)


async def pump(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, ws) -> None:
    """Carry bytes both ways until either end closes, then close both."""

    async def tcp_to_ws():
        while True:
            data = await reader.read(CHUNK)
            if not data:
                return
            await ws.send(data)

    async def ws_to_tcp():
        async for msg in ws:
            if isinstance(msg, str):
                return
            writer.write(msg)
            await writer.drain()

    tasks = [asyncio.create_task(tcp_to_ws()), asyncio.create_task(ws_to_tcp())]
    try:
        await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for task in tasks:
            task.cancel()
        for task in tasks:
            with contextlib.suppress(BaseException):
                await task
        with contextlib.suppress(Exception):
            await ws.close()
        writer.close()
        with contextlib.suppress(Exception):
            await writer.wait_closed()


def log(*args) -> None:
    print(*args, flush=True)


class Host:
    def __init__(self, server: str, room: dict, game: tuple[str, int], roster: Optional[Path] = None):
        self.server = server.rstrip("/")
        self.ws = ws_base(server)
        self.room = room  # the create request body
        self.game = game
        self.roster = roster
        self.room_id = ""
        self.code = ""
        self.token = ""
        self.tunnels: dict[str, asyncio.Task] = {}
        self.ready = asyncio.Event()

    async def create(self, http: httpx.AsyncClient) -> None:
        r = await http.post(self.server + "/api/rooms", json=self.room)
        r.raise_for_status()
        body = r.json()
        self.room_id, self.code, self.token = body["room_id"], body["code"], body["host_token"]
        log(f"room created: code {self.code}")

    async def delete(self, http: httpx.AsyncClient) -> None:
        with contextlib.suppress(Exception):
            await http.delete(f"{self.server}/api/rooms/{self.room_id}",
                              headers={"Authorization": "Bearer " + self.token})

    def state(self) -> dict:
        msg = {"type": "state", "players": []}
        if self.roster and self.roster.exists():
            with contextlib.suppress(Exception):
                data = json.loads(self.roster.read_text(encoding="utf-8"))
                msg.update({k: data[k] for k in ("players", "difficulty", "mode", "act", "vr") if k in data})
        return msg

    async def run_tunnel(self, tunnel_id: str) -> None:
        try:
            reader, writer = await asyncio.open_connection(*self.game)
        except OSError as e:
            log(f"tunnel {tunnel_id}: the game at {self.game[0]}:{self.game[1]} does not answer ({e})")
            # Attach and close at once so the joiner learns now, not after a timeout.
            with contextlib.suppress(Exception):
                async with connect(f"{self.ws}/ws/data/{tunnel_id}?side=host&token={self.token}"):
                    pass
            return
        try:
            ws = await connect(f"{self.ws}/ws/data/{tunnel_id}?side=host&token={self.token}")
        except Exception as e:
            log(f"tunnel {tunnel_id}: relay refused ({e})")
            writer.close()
            return
        log(f"tunnel {tunnel_id}: up")
        await pump(reader, writer, ws)
        log(f"tunnel {tunnel_id}: closed")

    def start_tunnel(self, tunnel_id: str) -> None:
        if tunnel_id not in self.tunnels:
            task = asyncio.create_task(self.run_tunnel(tunnel_id))
            self.tunnels[tunnel_id] = task
            task.add_done_callback(lambda _t, tid=tunnel_id: self.tunnels.pop(tid, None))

    async def control(self) -> None:
        """The control socket, reconnected while the room lives."""
        url = f"{self.ws}/ws/host/{self.room_id}?token={self.token}"
        delay = 1.0
        while True:
            try:
                async with connect(url) as ws:
                    delay = 1.0
                    sender = asyncio.create_task(self._send_state(ws))
                    try:
                        async for raw in ws:
                            msg = json.loads(raw)
                            kind = msg.get("type")
                            if kind == "hello":
                                self.ready.set()
                                for tid in msg.get("tunnels", []):
                                    self.start_tunnel(tid)
                            elif kind == "tunnel":
                                log(f"joiner ({msg.get('mod', {}).get('id', '?')}) -> tunnel {msg['tunnel_id']}")
                                self.start_tunnel(msg["tunnel_id"])
                            elif kind == "closed":
                                log(f"room closed by the server: {msg.get('reason')}")
                                return
                    finally:
                        sender.cancel()
                    if ws.close_code == CLOSE_REPLACED:
                        log("another helper took this room over")
                        return
            except InvalidStatus as e:
                # 403: the server refused the socket - the room is gone or the
                # token is wrong; retrying cannot help. Anything else (a proxy's
                # 502 during a restart) is retried.
                if e.response.status_code == 403:
                    log("room no longer exists on the server")
                    return
                log(f"control: {e}")
            except (ConnectionClosed, OSError) as e:
                log(f"control: {e}")
            await asyncio.sleep(delay)
            delay = min(delay * 2, 8.0)

    async def _send_state(self, ws) -> None:
        while True:
            await ws.send(json.dumps(self.state()))
            await asyncio.sleep(STATE_EVERY)


class Joiner:
    def __init__(self, server: str, target: dict, listen: tuple[str, int]):
        self.server = server.rstrip("/")
        self.ws = ws_base(server)
        self.target = target  # the join request body minus nothing: room_id/code, password, mod, player
        self.listen = listen
        self.http: Optional[httpx.AsyncClient] = None
        self.server_obj: Optional[asyncio.base_events.Server] = None

    async def on_game(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        assert self.http is not None
        try:
            r = await self.http.post(self.server + "/api/join", json=self.target)
        except Exception as e:
            log(f"join failed: {e}")
            writer.close()
            return
        if r.status_code != 200:
            log(f"join refused: {r.status_code} {r.text}")
            writer.close()
            return
        body = r.json()
        if body.get("mod_mismatch"):
            log("note: the host plays another mod")
        try:
            ws = await connect(f"{self.ws}/ws/data/{body['tunnel_id']}?side=join&token={body['join_token']}")
        except Exception as e:
            log(f"relay refused: {e}")
            writer.close()
            return
        log(f"tunnel {body['tunnel_id']}: up")
        await pump(reader, writer, ws)
        log(f"tunnel {body['tunnel_id']}: closed")

    async def start(self, http: httpx.AsyncClient) -> int:
        """Starts listening; returns the bound port (useful when --listen asks for port 0)."""
        self.http = http
        self.server_obj = await asyncio.start_server(self.on_game, *self.listen)
        return self.server_obj.sockets[0].getsockname()[1]


async def cmd_list(args) -> None:
    async with httpx.AsyncClient(timeout=10) as http:
        r = await http.get(args.server.rstrip("/") + "/api/rooms")
        r.raise_for_status()
        for room in r.json()["rooms"]:
            lock = " [locked]" if room["locked"] else ""
            log(f"{room['code']}  {room['name']}{lock}  {room['difficulty']}  {room['mod']['id']} "
                f"{room['mod']['version']}  {room['players_count']}/{room['max_players']}")
            for p in room["players"]:
                mod = p["mod"]["id"] if p["mod"] else "?"
                log(f"      {p['name']}  {p['class']} {p['level'] or ''}  act {p['act'] or '?'} {p['area']}  [{mod}]")


async def cmd_host(args) -> None:
    room = {"name": args.name, "mod": {"id": args.mod, "version": args.version},
            "difficulty": args.difficulty, "mode": args.mode, "public": not args.private,
            "password": args.password}
    host = Host(args.server, room, split_addr(args.game), Path(args.roster) if args.roster else None)
    async with httpx.AsyncClient(timeout=10) as http:
        await host.create(http)
        try:
            await host.control()
        finally:
            await host.delete(http)


async def cmd_join(args) -> None:
    target = {"mod": {"id": args.mod, "version": args.version}, "password": args.password}
    if args.code:
        target["code"] = args.code
    else:
        target["room_id"] = args.room
    if args.char:
        target["player"] = {"name": args.char, "class": args.char_class, "level": args.level}
    joiner = Joiner(args.server, target, split_addr(args.listen))
    async with httpx.AsyncClient(timeout=10) as http:
        port = await joiner.start(http)
        log(f"listening on {joiner.listen[0]}:{port} - in D2R join the TCP/IP game 127.0.0.1")
        await asyncio.Event().wait()


def main(argv=None) -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--server", required=True)
        return sp

    common(sub.add_parser("list"))

    h = common(sub.add_parser("host"))
    h.add_argument("--name", required=True)
    h.add_argument("--mod", default="none")
    h.add_argument("--version", default="")
    h.add_argument("--difficulty", default="normal", choices=["normal", "nightmare", "hell"])
    h.add_argument("--mode", default="softcore", choices=["softcore", "hardcore"])
    h.add_argument("--password", default="")
    h.add_argument("--private", action="store_true")
    h.add_argument("--game", default="127.0.0.1:4000")
    h.add_argument("--roster", help="JSON file with players/difficulty/act, re-read every 5 s")

    j = common(sub.add_parser("join"))
    g = j.add_mutually_exclusive_group(required=True)
    g.add_argument("--code")
    g.add_argument("--room")
    j.add_argument("--mod", default="none")
    j.add_argument("--version", default="")
    j.add_argument("--password", default="")
    j.add_argument("--listen", default="127.0.0.1:4000")
    j.add_argument("--char", help="your character name, shown in the room list")
    j.add_argument("--char-class", default="unknown")
    j.add_argument("--level", type=int)

    args = p.parse_args(argv)
    run = {"list": cmd_list, "host": cmd_host, "join": cmd_join}[args.cmd]
    with contextlib.suppress(KeyboardInterrupt):
        asyncio.run(run(args))


if __name__ == "__main__":
    sys.exit(main())
