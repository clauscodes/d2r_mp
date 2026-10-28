"""The Python relay against relay/conformance.py, open and with keys, plus its
rate limit. Plain unittest: python -m unittest relay/python/test_relay.py"""

import asyncio
import os
import socket
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

import conformance  # noqa: E402
import relay_server as relay  # noqa: E402


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class RelayTest(unittest.IsolatedAsyncioTestCase):
    async def start(self, **kw) -> str:
        port = free_port()
        settings = relay.Settings(host="127.0.0.1", port=port, **kw)
        ready = asyncio.get_running_loop().create_future()
        self.task = asyncio.create_task(relay.run(settings, ready))
        self.relay = await ready
        return f"ws://127.0.0.1:{port}"

    async def asyncTearDown(self) -> None:
        self.task.cancel()
        try:
            await self.task
        except asyncio.CancelledError:
            pass

    def assertAllPassed(self, results) -> None:
        failed = [f"{n}: {e}" for n, e in results if e]
        self.assertEqual(failed, [], "\n".join(failed))

    async def test_conformance_open(self) -> None:
        self.assertAllPassed(await conformance.run_all(await self.start()))

    async def test_conformance_keys(self) -> None:
        url = await self.start(keys=("friends-key-1234567",))
        self.assertAllPassed(await conformance.run_all(url, "friends-key-1234567"))

    async def test_closed_relay_refuses_new_pipes(self) -> None:
        url = await self.start()
        r = conformance.Relay(url, None)
        room = conformance.Room()
        host = await r.host(room.id, room.secret, room.key)
        await conformance.hello(host)
        j = await r.join(room.id, room.key)
        await conformance.hello(j)
        self.relay.s.open = False  # the cap is reached while a game runs
        await j.send(b"still flowing")
        self.assertEqual(await conformance.frame(host), (conformance.OPEN, 1, b""))
        self.assertEqual(await conformance.frame(host), (conformance.DATA, 1, b"still flowing"))
        other = conformance.Room()
        status, err = await conformance.refused(lambda: r.host(other.id, other.secret, other.key))
        self.assertEqual((status, err), (503, "near_limit"))
        status, err = await conformance.refused(lambda: r.join(room.id, room.key))
        self.assertEqual((status, err), (503, "over_budget"))
        _, health = await r.get("/healthz")
        self.assertEqual((health["status"], health["open"]), ("limit", False))
        await j.close()
        await host.close()

    async def test_rate_limit_closes_the_pipe(self) -> None:
        url = await self.start(rate=10_000, burst=20_000)
        r = conformance.Relay(url, None)
        room = conformance.Room()
        host = await r.host(room.id, room.secret, room.key)
        await conformance.hello(host)
        j = await r.join(room.id, room.key)
        await conformance.hello(j)
        self.assertEqual(await conformance.frame(host), (conformance.OPEN, 1, b""))
        for _ in range(4):
            await j.send(b"x" * 8000)
        frames = [await conformance.frame(host) for _ in range(3)]
        self.assertEqual([f[0] for f in frames[:2]], [conformance.DATA, conformance.DATA])
        self.assertEqual(frames[2], (conformance.CLOSE, 1, b"rate"))
        code, reason = await conformance.closed(j)
        self.assertEqual((code, reason), (1000, "rate"))
        await host.close()

    async def test_status_levels(self) -> None:
        url = await self.start(max_rooms=5, near_limit=0.4)
        r = conformance.Relay(url, None)
        rooms = [conformance.Room() for _ in range(6)]
        hosts = []

        async def status() -> str:
            return (await r.get("/healthz"))[1]["status"]

        self.assertEqual(await status(), "active")
        for room in rooms[:2]:
            hosts.append(await r.host(room.id, room.secret, room.key))
            await conformance.hello(hosts[-1])
        self.assertEqual(await status(), "near_limit")
        # Near the limit: no new room, but the rooms already made take joiners
        # and their hosts may reconnect.
        st, err = await conformance.refused(lambda: r.host(rooms[2].id, rooms[2].secret, rooms[2].key))
        self.assertEqual((st, err), (503, "near_limit"))
        j = await r.join(rooms[0].id, rooms[0].key)
        await conformance.hello(j)
        again = await r.host(rooms[1].id, rooms[1].secret, rooms[1].key)
        await conformance.hello(again)
        await j.close()
        for h in hosts + [again]:
            await h.close()


if __name__ == "__main__":
    unittest.main()
