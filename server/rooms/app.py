"""HTTP API and WebSockets (SPEC §5).

Run with ONE worker: rooms and tunnels live in this process's memory.
    uvicorn rooms.app:app --workers 1 --no-access-log
The access log is off because it would write every client's address.
"""

import asyncio
import json
import time
from contextlib import asynccontextmanager
from typing import Optional

from fastapi import FastAPI, Request, WebSocket
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from pydantic import ValidationError

from . import PROTOCOL_VERSION
from .config import Settings
from .models import CreateRoom, HostState, JoinRoom
from .registry import SIDES, AddressHasher, LimitError, Registry, TokenBucket, same_token

CONTROL_MSG_MAX = 16 * 1024

# WebSocket close codes of our own (4000-4999 are the application's).
CLOSE_REPLACED = 4000
CLOSE_BAD_TOKEN = 4401
CLOSE_NOT_FOUND = 4404
CLOSE_TAKEN = 4409


def error(status: int, code: str, **extra) -> JSONResponse:
    return JSONResponse({"v": PROTOCOL_VERSION, "error": code, **extra}, status_code=status)


def create_app(settings: Optional[Settings] = None) -> FastAPI:
    s = settings or Settings.from_env()
    reg = Registry(s)
    addr_hash = AddressHasher()

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        sweeper = asyncio.create_task(reg.sweep_forever())
        try:
            yield
        finally:
            sweeper.cancel()
            for room in list(reg.rooms.values()):
                await reg.close_room(room, "server_stopping")

    app = FastAPI(title="D2R Rooms", lifespan=lifespan, docs_url=None, redoc_url=None, openapi_url=None)
    app.state.registry = reg
    app.state.settings = s

    def source(conn: Request | WebSocket) -> str:
        addr = conn.client.host if conn.client else "?"
        if s.trust_proxy:
            fwd = conn.headers.get("x-forwarded-for", "")
            if fwd.strip():
                addr = fwd.split(",")[-1].strip()
        return addr_hash(addr)

    @app.exception_handler(RequestValidationError)
    async def _invalid(request: Request, exc: RequestValidationError):
        # Only where and what - never the input, which may hold a password.
        detail = [{"loc": list(e.get("loc", ())), "msg": e.get("msg", "")} for e in exc.errors()]
        return error(422, "invalid", detail=detail)

    @app.exception_handler(LimitError)
    async def _limit(request: Request, exc: LimitError):
        return error(exc.status, exc.error)

    # --- HTTP --------------------------------------------------------------

    @app.get("/healthz")
    async def healthz():
        return {"ok": True, "rooms": len(reg.rooms), "tunnels": len(reg.tunnels)}

    @app.get("/api/rooms")
    async def list_rooms(mod: Optional[str] = None, difficulty: Optional[str] = None):
        return {"v": PROTOCOL_VERSION, "rooms": reg.list_public(mod, difficulty)}

    @app.get("/api/rooms/by-code/{code}")
    async def room_by_code(code: str):
        room = reg.find(code=code)
        if room is None:
            return error(404, "no_room")
        return {"v": PROTOCOL_VERSION, "room": room.public_view()}

    @app.post("/api/rooms")
    async def create_room(body: CreateRoom, request: Request):
        room = reg.create_room(body, source(request))
        return {"v": PROTOCOL_VERSION, "room_id": room.room_id, "code": room.code, "host_token": room.host_token}

    @app.delete("/api/rooms/{room_id}")
    async def delete_room(room_id: str, request: Request):
        room = reg.find(room_id=room_id)
        token = request.headers.get("authorization", "").removeprefix("Bearer ").strip()
        if room is None:
            return error(404, "no_room")
        if not same_token(token, room.host_token):
            return error(403, "bad_token")
        await reg.close_room(room, "host_closed")
        return {"v": PROTOCOL_VERSION, "ok": True}

    @app.post("/api/join")
    async def join(body: JoinRoom, request: Request):
        room = reg.find(room_id=body.room_id, code=body.code)
        if room is None:
            return error(404, "no_room")
        key = (source(request), room.room_id)
        if reg.join_fails.count(key, s.join_fail_window) >= s.join_fail_limit:
            return error(429, "too_many_attempts")
        if not room.password_ok(body.password):
            reg.join_fails.add(key)
            return error(403, "bad_password")
        t = reg.open_tunnel(room, body.mod, body.player)
        await room.send_host({"type": "tunnel", "tunnel_id": t.tunnel_id, "mod": body.mod.model_dump()})
        return {
            "v": PROTOCOL_VERSION,
            "tunnel_id": t.tunnel_id,
            "join_token": t.join_token,
            "room_id": room.room_id,
            "mod_mismatch": body.mod.id != room.mod.id,
        }

    # --- host control socket -------------------------------------------------

    @app.websocket("/ws/host/{room_id}")
    async def host_socket(ws: WebSocket, room_id: str, token: str = ""):
        room = reg.find(room_id=room_id)
        if room is None:
            await ws.close(code=CLOSE_NOT_FOUND)
            return
        if not same_token(token, room.host_token):
            await ws.close(code=CLOSE_BAD_TOKEN)
            return
        await ws.accept()
        async with room.control_lock:
            old, room.control = room.control, ws
        if old is not None:
            try:
                await old.close(code=CLOSE_REPLACED)
            except Exception:
                pass
        room.host_lost_at = None
        room.last_state = time.monotonic()
        # Tunnels opened while the host was away still wait for its end.
        pending = [t.tunnel_id for t in room.tunnels.values() if "host" not in t.sockets]
        await room.send_host({"type": "hello", "v": PROTOCOL_VERSION, "room_id": room.room_id,
                              "code": room.code, "tunnels": pending})
        try:
            while not room.closed:
                msg = await ws.receive()
                if msg["type"] == "websocket.disconnect":
                    break
                text = msg.get("text")
                if text is None or len(text) > CONTROL_MSG_MAX:
                    await ws.close(code=1003 if text is None else 1009)
                    break
                try:
                    data = json.loads(text)
                    kind = data.get("type") if isinstance(data, dict) else None
                except ValueError:
                    kind = None
                if kind == "state":
                    try:
                        room.apply_state(HostState.model_validate(data))
                    except ValidationError:
                        await room.send_host({"type": "error", "error": "bad_state"})
                elif kind == "kick":
                    t = room.tunnels.get(str(data.get("tunnel_id", "")))
                    if t is not None:
                        await reg.close_tunnel(t)
                elif kind == "ping":
                    await room.send_host({"type": "pong"})
                else:
                    await room.send_host({"type": "error", "error": "unknown_message"})
        except Exception:
            pass
        finally:
            async with room.control_lock:
                if room.control is ws:
                    room.control = None
                    room.host_lost_at = time.monotonic()

    # --- tunnel data sockets ---------------------------------------------------

    @app.websocket("/ws/data/{tunnel_id}")
    async def data_socket(ws: WebSocket, tunnel_id: str, side: str = "", token: str = ""):
        t = reg.tunnels.get(tunnel_id)
        if t is None or side not in SIDES:
            await ws.close(code=CLOSE_NOT_FOUND)
            return
        if not t.token_ok(side, token):
            await ws.close(code=CLOSE_BAD_TOKEN)
            return
        if side in t.sockets:
            await ws.close(code=CLOSE_TAKEN)
            return
        await ws.accept()
        t.attach(side, ws)
        close_code = 1000
        try:
            # Nothing is read until the other end is there: the sender's TCP
            # window fills and it waits, so no bytes are buffered here.
            try:
                await asyncio.wait_for(t.both.wait(), s.tunnel_connect_timeout)
            except asyncio.TimeoutError:
                return
            if t.closed:
                return
            peer = t.peer(side)
            bucket = TokenBucket(s.tunnel_rate, s.frame_max)
            while not t.closed:
                msg = await ws.receive()
                if msg["type"] == "websocket.disconnect":
                    break
                data = msg.get("bytes")
                if data is None:
                    close_code = 1003
                    break
                if len(data) > s.frame_max:
                    close_code = 1009
                    break
                await bucket.take(len(data))
                await peer.send_bytes(data)
        except Exception:
            pass
        finally:
            await reg.close_tunnel(t, code=close_code)

    return app


app = create_app()
