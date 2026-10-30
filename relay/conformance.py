"""Checks a running relay against relay/PROTOCOL.md v1.

    python relay/conformance.py ws://127.0.0.1:8080
    python relay/conformance.py wss://my-relay.example.workers.dev --relay-key KEY

Needs only `websockets` (pip install websockets). It plays a fake host and fake
joiners, so no game is needed. Exit code 0 when every check passed. The Python
relay's tests and the Cloudflare relay's local check both run it, which is what
keeps the two servers interchangeable.
"""

import argparse
import asyncio
import hashlib
import json
import secrets
import sys
import urllib.error
import urllib.request
from typing import Awaitable, Callable, Optional

from websockets.asyncio.client import ClientConnection, connect
from websockets.exceptions import ConnectionClosed, InvalidStatus

OPEN, DATA, CLOSE = 1, 2, 3
PING, PONG = '{"t":"ping"}', '{"t":"pong"}'
TIMEOUT = 5.0


class Fail(Exception):
    pass


def check(ok: bool, what: str) -> None:
    if not ok:
        raise Fail(what)


def token() -> str:
    return secrets.token_urlsafe(24)


class Relay:
    def __init__(self, url: str, relay_key: Optional[str]) -> None:
        self.url = url.rstrip("/")
        self.relay_key = relay_key

    def http_url(self, path: str) -> str:
        return self.url.replace("wss://", "https://", 1).replace("ws://", "http://", 1) + path

    async def get(self, path: str, headers: Optional[dict] = None) -> tuple[int, dict]:
        # In a thread: the relay under test may run on this same event loop.
        return await asyncio.to_thread(self._get, path, headers)

    def _get(self, path: str, headers: Optional[dict] = None) -> tuple[int, dict]:
        req = urllib.request.Request(self.http_url(path), headers={"User-Agent": "d2r-rooms-conformance", **(headers or {})})
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
                return r.status, json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            with e:
                body = e.read()
            try:
                return e.code, json.loads(body or b"{}")
            except ValueError:
                return e.code, {}

    async def host(self, room: str, secret: str, join_key: str, relay_key: Optional[str] = "default") -> ClientConnection:
        headers = {"X-Host-Secret": secret, "X-Join-Key-Hash": hashlib.sha256(join_key.encode()).hexdigest()}
        key = self.relay_key if relay_key == "default" else relay_key
        if key:
            headers["X-Relay-Key"] = key
        return await connect(f"{self.url}/v1/rooms/{room}/host", additional_headers=headers, compression=None, open_timeout=TIMEOUT)

    async def join(self, room: str, join_key: str) -> ClientConnection:
        return await connect(f"{self.url}/v1/rooms/{room}/join", additional_headers={"X-Join-Key": join_key},
                             compression=None, open_timeout=TIMEOUT)


async def recv(ws: ClientConnection):
    return await asyncio.wait_for(ws.recv(), TIMEOUT)


async def hello(ws: ClientConnection) -> dict:
    msg = await recv(ws)
    check(isinstance(msg, str), "hello is a text message")
    h = json.loads(msg)
    check(h.get("t") == "hello" and h.get("v") == 1, f"hello v1, got {h}")
    return h


async def frame(ws: ClientConnection) -> tuple[int, int, bytes]:
    while True:
        msg = await recv(ws)
        if isinstance(msg, str):
            continue  # pongs and future text messages
        check(len(msg) >= 2, "a frame has a type and a pipe byte")
        return msg[0], msg[1], bytes(msg[2:])


async def data(ws: ClientConnection) -> bytes:
    while True:
        msg = await recv(ws)
        if isinstance(msg, bytes):
            return msg


async def closed(ws: ClientConnection) -> tuple[Optional[int], str]:
    # The relay's close frame is what counts. After it the client waits for the
    # server to drop TCP, which Cloudflare's edge does only seconds later, so a
    # received close frame ends the wait here.
    deadline = asyncio.get_running_loop().time() + TIMEOUT
    try:
        while True:
            rcvd = ws.protocol.close_rcvd
            if rcvd is not None:
                ws.transport.abort()
                return rcvd.code, rcvd.reason
            left = deadline - asyncio.get_running_loop().time()
            if left <= 0:
                raise asyncio.TimeoutError()
            try:
                await asyncio.wait_for(ws.recv(), min(0.1, left))
            except asyncio.TimeoutError:
                pass
    except ConnectionClosed as e:
        return (e.rcvd.code if e.rcvd else None), (e.rcvd.reason if e.rcvd else "")


async def refused(make: Callable[[], Awaitable[ClientConnection]]) -> tuple[int, str]:
    try:
        ws = await make()
    except InvalidStatus as e:
        try:
            err = json.loads(e.response.body or b"{}").get("error", "")
        except ValueError:
            err = ""
        return e.response.status_code, err
    await ws.close()
    raise Fail("the upgrade was accepted")


class Room:
    """A fresh room id, host secret and join key."""

    def __init__(self) -> None:
        self.id, self.secret, self.key = token(), token(), token()


# --- the checks -------------------------------------------------------------


async def c_health(r: Relay) -> None:
    status, h = await r.get("/healthz")
    check(status == 200, f"/healthz 200, got {status}")
    check(h.get("ok") is True and h.get("relay") == 1, f"/healthz ok + relay 1, got {h}")
    check(isinstance(h.get("open"), bool) and h.get("pipes") == 7 and h.get("max_message") == 65536, f"/healthz fields, got {h}")
    check(h.get("status") in ("active", "near_limit", "limit"), f"/healthz status, got {h}")
    check(h["open"] == (h["status"] != "limit"), f"open is false only at the limit, got {h}")
    u = h.get("usage")
    check(u is None or (isinstance(u, (int, float)) and u >= 0), f"usage is a number >= 0, got {h}")


async def c_plain_http(r: Relay) -> None:
    status, h = await r.get(f"/v1/rooms/{token()}/join")
    check(status == 426 and h.get("error") == "upgrade_required", f"426 upgrade_required, got {status} {h}")
    status, h = await r.get("/nothing-here")
    check(status == 404, f"404 for an unknown path, got {status}")


async def c_no_room(r: Relay) -> None:
    room = Room()
    status, err = await refused(lambda: r.join(room.id, room.key))
    check((status, err) == (404, "no_room"), f"404 no_room, got {status} {err}")


async def c_bad_request(r: Relay) -> None:
    status, err = await refused(lambda: r.join("short", token()))
    check(status in (400, 404), f"a short room id is refused, got {status} {err}")
    room = Room()
    status, err = await refused(lambda: r.host(room.id, "bad secret!", room.key))
    check((status, err) == (400, "bad_request"), f"400 bad_request, got {status} {err}")


async def c_pipe_roundtrip(r: Relay) -> None:
    room = Room()
    host = await r.host(room.id, room.secret, room.key)
    h = await hello(host)
    check(h.get("pipes") == 7, "host hello has pipes")
    joiner = await r.join(room.id, room.key)
    jh = await hello(joiner)
    check(jh.get("pipe") == 1, f"first pipe is 1, got {jh}")
    check(await frame(host) == (OPEN, 1, b""), "host gets OPEN 1")
    await joiner.send(b"\x01hello from the joiner")
    check(await frame(host) == (DATA, 1, b"\x01hello from the joiner"), "host gets the joiner's bytes in a DATA frame")
    await host.send(bytes([DATA, 1]) + b"hello from the host")
    check(await data(joiner) == b"hello from the host", "joiner gets the host's bytes unframed")
    big = secrets.token_bytes(60000)
    await joiner.send(big)
    check(await frame(host) == (DATA, 1, big), "60 KB up arrives whole")
    await host.send(bytes([DATA, 1]) + big)
    check(await data(joiner) == big, "60 KB down arrives whole")
    # Ping on both sockets.
    await host.send(PING)
    check(await recv(host) == PONG, "host ping -> pong")
    await joiner.send(PING)
    check(await recv(joiner) == PONG, "joiner ping -> pong")
    # Ordering: many small messages arrive in order.
    for i in range(200):
        await joiner.send(i.to_bytes(2, "big"))
    got = [int.from_bytes((await frame(host))[2], "big") for _ in range(200)]
    check(got == list(range(200)), "200 messages keep their order")
    await joiner.close()
    check(await frame(host) == (CLOSE, 1, b""), "joiner leaving -> host gets CLOSE 1")
    await host.close()


async def c_pipes_and_close(r: Relay) -> None:
    room = Room()
    host = await r.host(room.id, room.secret, room.key)
    await hello(host)
    joiners = []
    for n in range(1, 8):
        j = await r.join(room.id, room.key)
        check((await hello(j)).get("pipe") == n, f"pipe {n}")
        check(await frame(host) == (OPEN, n, b""), f"OPEN {n}")
        joiners.append(j)
    status, err = await refused(lambda: r.join(room.id, room.key))
    check((status, err) == (409, "room_full"), f"an 8th joiner gets 409 room_full, got {status} {err}")
    # The host ends pipe 2; the relay closes that joiner.
    await host.send(bytes([CLOSE, 2]) + b"bye")
    code, reason = await closed(joiners[1])
    check(code == 1000, f"host CLOSE -> joiner closed 1000, got {code} {reason}")
    # Its number is free again and goes to the next joiner.
    j = await r.join(room.id, room.key)
    check((await hello(j)).get("pipe") == 2, "a freed pipe number is reused")
    check(await frame(host) == (OPEN, 2, b""), "OPEN 2 again")
    # DATA for a pipe that is not open is ignored, not fatal.
    await host.send(bytes([DATA, 200]) + b"nobody")
    await host.send(PING)
    check(await recv(host) == PONG, "the host socket lives after DATA to a closed pipe")
    # The host leaving closes everyone.
    await host.close()
    code, _ = await closed(j)
    check(code == 1001, f"host gone -> joiners closed 1001, got {code}")
    for other in joiners[2:]:
        await closed(other)
    status, err = await refused(lambda: r.join(room.id, room.key))
    check((status, err) == (404, "no_room"), f"after the host left: 404 no_room, got {status} {err}")


async def c_keys(r: Relay) -> None:
    room = Room()
    host = await r.host(room.id, room.secret, room.key)
    await hello(host)
    status, err = await refused(lambda: r.join(room.id, token()))
    check((status, err) == (403, "bad_join_key"), f"wrong join key: 403 bad_join_key, got {status} {err}")
    status, err = await refused(lambda: r.host(room.id, token(), room.key))
    check((status, err) == (403, "bad_secret"), f"another host secret: 403 bad_secret, got {status} {err}")
    # The same secret replaces the host socket.
    host2 = await r.host(room.id, room.secret, room.key)
    await hello(host2)
    code, reason = await closed(host)
    check(code == 4000, f"replaced host closed 4000, got {code} {reason}")
    j = await r.join(room.id, room.key)
    await hello(j)
    check(await frame(host2) == (OPEN, 1, b""), "the new host socket gets the pipes")
    await host2.close()
    await closed(j)
    # The claim outlives the host socket: another secret is still refused.
    status, err = await refused(lambda: r.host(room.id, token(), room.key))
    check((status, err) == (403, "bad_secret"), f"claim kept after the host left, got {status} {err}")
    host3 = await r.host(room.id, room.secret, room.key)
    await hello(host3)
    await host3.close()


async def c_relay_key(r: Relay) -> None:
    room = Room()
    if not r.relay_key:
        # An open relay accepts any host without a key.
        host = await r.host(room.id, room.secret, room.key, relay_key=None)
        await hello(host)
        await host.close()
        return
    status, err = await refused(lambda: r.host(room.id, room.secret, room.key, relay_key=None))
    check((status, err) == (401, "relay_key"), f"no relay key: 401 relay_key, got {status} {err}")
    status, err = await refused(lambda: r.host(room.id, room.secret, room.key, relay_key="wrong-key-wrong-key"))
    check((status, err) == (401, "relay_key"), f"wrong relay key: 401, got {status} {err}")


CHECKS = [c_health, c_plain_http, c_no_room, c_bad_request, c_pipe_roundtrip, c_pipes_and_close, c_keys, c_relay_key]


async def run_all(url: str, relay_key: Optional[str] = None) -> list[tuple[str, Optional[str]]]:
    r = Relay(url, relay_key)
    results = []
    for c in CHECKS:
        try:
            await asyncio.wait_for(c(r), 60)
            results.append((c.__name__[2:], None))
        except Exception as e:  # report every check, not only the first failure
            results.append((c.__name__[2:], f"{type(e).__name__}: {e}"))
    return results


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("url", help="ws://… or wss://… (no path)")
    p.add_argument("--relay-key", default=None, help="the key, when the relay needs one for hosts")
    a = p.parse_args()
    results = asyncio.run(run_all(a.url, a.relay_key))
    for name, err in results:
        print(f"{'ok  ' if err is None else 'FAIL'} {name}{'' if err is None else ': ' + err}")
    failed = sum(1 for _, e in results if e)
    print(f"{len(results) - failed}/{len(results)} checks passed against {a.url}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
