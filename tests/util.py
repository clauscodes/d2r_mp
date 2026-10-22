"""Small client-side helpers shared by the tests."""

import asyncio
import json

import httpx
from websockets.asyncio.client import connect

ROOM = {"name": "Baal runs", "mod": {"id": "d2r-vr", "version": "0.175.0"}, "difficulty": "hell"}
JOIN_MOD = {"id": "d2r-pcvr", "version": "0.3.19"}


def run(coro, timeout: float = 30):
    return asyncio.run(asyncio.wait_for(coro, timeout))


def create(srv, **over) -> dict:
    body = {**ROOM, **over}
    r = httpx.post(srv.http + "/api/rooms", json=body)
    assert r.status_code == 200, r.text
    return r.json()


def by_code(srv, code: str) -> httpx.Response:
    return httpx.get(f"{srv.http}/api/rooms/by-code/{code}")


async def ajoin(srv, room: dict, **over) -> httpx.Response:
    body = {"room_id": room["room_id"], "mod": JOIN_MOD, **over}
    async with httpx.AsyncClient() as http:
        return await http.post(srv.http + "/api/join", json=body)


async def alist(srv, **params) -> list[dict]:
    async with httpx.AsyncClient() as http:
        r = await http.get(srv.http + "/api/rooms", params=params)
        r.raise_for_status()
        return r.json()["rooms"]


class Control:
    """The host's control socket, with a queue of what the server sent."""

    def __init__(self, srv, room: dict):
        self.url = f"{srv.ws}/ws/host/{room['room_id']}?token={room['host_token']}"
        self.ws = None
        self.msgs: asyncio.Queue = asyncio.Queue()
        self._reader = None

    async def __aenter__(self):
        self.ws = await connect(self.url)
        self._reader = asyncio.create_task(self._read())
        hello = await self.expect("hello")
        self.hello = hello
        return self

    async def __aexit__(self, *exc):
        self._reader.cancel()
        await self.ws.close()

    async def _read(self):
        try:
            async for raw in self.ws:
                await self.msgs.put(json.loads(raw))
        except Exception:
            pass

    async def send(self, msg: dict):
        await self.ws.send(json.dumps(msg))

    async def expect(self, kind: str, timeout: float = 5) -> dict:
        while True:
            msg = await asyncio.wait_for(self.msgs.get(), timeout)
            if msg.get("type") == kind:
                return msg


def data_url(srv, tunnel_id: str, side: str, token: str) -> str:
    return f"{srv.ws}/ws/data/{tunnel_id}?side={side}&token={token}"


async def recv_exactly(ws, n: int, timeout: float = 10) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        buf += await asyncio.wait_for(ws.recv(), timeout)
    return bytes(buf)
