"""The relay: a fake tunnel made of two raw data sockets, no game involved."""

import asyncio
import os
import time

import pytest
from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed, InvalidStatus

from util import Control, ajoin, create, data_url, recv_exactly, run


async def open_tunnel(srv, room, ctl, **join):
    r = await ajoin(srv, room, **join)
    assert r.status_code == 200, r.text
    body = r.json()
    msg = await ctl.expect("tunnel")
    assert msg["tunnel_id"] == body["tunnel_id"]
    return body


def test_bytes_both_ways_in_order(live):
    srv = live(tunnel_rate=64 * 1024 * 1024)
    room = create(srv)
    up = os.urandom(1024 * 1024)
    down = os.urandom(700 * 1000)

    async def go():
        async with Control(srv, room) as ctl:
            t = await open_tunnel(srv, room, ctl)
            # The joiner attaches and starts sending before the host is there.
            join = await connect(data_url(srv, t["tunnel_id"], "join", t["join_token"]))

            async def send(ws, data):
                for i in range(0, len(data), 16 * 1024):
                    await ws.send(data[i:i + 16 * 1024])

            early = asyncio.create_task(send(join, up))
            await asyncio.sleep(0.2)
            host = await connect(data_url(srv, t["tunnel_id"], "host", room["host_token"]))
            late = asyncio.create_task(send(host, down))
            got_up, got_down = await asyncio.gather(recv_exactly(host, len(up)), recv_exactly(join, len(down)))
            await asyncio.gather(early, late)
            assert got_up == up and got_down == down

            await join.close()
            await asyncio.wait_for(host.wait_closed(), 5)
            left = await ctl.expect("left")
            assert left["tunnel_id"] == t["tunnel_id"]

    run(go())
    assert srv.registry.tunnels == {}


def test_bad_token_and_second_attach_are_refused(live):
    srv = live()
    room = create(srv)

    async def go():
        async with Control(srv, room) as ctl:
            t = await open_tunnel(srv, room, ctl)
            for url in (data_url(srv, t["tunnel_id"], "join", room["host_token"]),
                        data_url(srv, t["tunnel_id"], "host", t["join_token"]),
                        data_url(srv, t["tunnel_id"], "middle", t["join_token"]),
                        data_url(srv, "nope", "join", t["join_token"])):
                with pytest.raises(InvalidStatus) as e:
                    await connect(url)
                assert e.value.response.status_code == 403
            first = await connect(data_url(srv, t["tunnel_id"], "join", t["join_token"]))
            with pytest.raises(InvalidStatus):
                await connect(data_url(srv, t["tunnel_id"], "join", t["join_token"]))
            await first.close()
            with pytest.raises(InvalidStatus):
                await connect(f"{srv.ws}/ws/host/{room['room_id']}?token={t['join_token']}")

    run(go())


def test_oversized_frame_closes_the_tunnel(live):
    srv = live(frame_max=4096)
    room = create(srv)

    async def go():
        async with Control(srv, room) as ctl:
            t = await open_tunnel(srv, room, ctl)
            join = await connect(data_url(srv, t["tunnel_id"], "join", t["join_token"]), max_size=None)
            host = await connect(data_url(srv, t["tunnel_id"], "host", room["host_token"]), max_size=None)
            await join.send(b"x" * 4096)
            assert await recv_exactly(host, 4096) == b"x" * 4096
            await join.send(b"y" * 5000)
            for ws in (join, host):
                await asyncio.wait_for(ws.wait_closed(), 5)
                assert ws.close_code == 1009

    run(go())


def test_text_frame_closes_the_tunnel(live):
    srv = live()
    room = create(srv)

    async def go():
        async with Control(srv, room) as ctl:
            t = await open_tunnel(srv, room, ctl)
            join = await connect(data_url(srv, t["tunnel_id"], "join", t["join_token"]))
            host = await connect(data_url(srv, t["tunnel_id"], "host", room["host_token"]))
            await join.send("hello")
            for ws in (join, host):
                await asyncio.wait_for(ws.wait_closed(), 5)
            assert host.close_code == 1003

    run(go())


def test_kick_closes_both_ends(live):
    srv = live()
    room = create(srv)

    async def go():
        async with Control(srv, room) as ctl:
            t = await open_tunnel(srv, room, ctl)
            join = await connect(data_url(srv, t["tunnel_id"], "join", t["join_token"]))
            host = await connect(data_url(srv, t["tunnel_id"], "host", room["host_token"]))
            await ctl.send({"type": "kick", "tunnel_id": t["tunnel_id"]})
            for ws in (join, host):
                await asyncio.wait_for(ws.wait_closed(), 5)
            await ctl.expect("left")

    run(go())


def test_unanswered_tunnel_times_out(live):
    srv = live(tunnel_connect_timeout=0.8)
    room = create(srv)

    async def go():
        async with Control(srv, room) as ctl:
            t = await open_tunnel(srv, room, ctl)
            join = await connect(data_url(srv, t["tunnel_id"], "join", t["join_token"]))
            started = time.monotonic()
            await asyncio.wait_for(join.wait_closed(), 5)
            assert time.monotonic() - started < 3
            await ctl.expect("left")

    run(go())
    assert srv.registry.tunnels == {}


def test_tunnel_rate_is_throttled(live):
    rate = 64 * 1024
    srv = live(tunnel_rate=rate, frame_max=16 * 1024)
    room = create(srv)
    payload = os.urandom(3 * rate)

    async def go():
        async with Control(srv, room) as ctl:
            t = await open_tunnel(srv, room, ctl)
            join = await connect(data_url(srv, t["tunnel_id"], "join", t["join_token"]))
            host = await connect(data_url(srv, t["tunnel_id"], "host", room["host_token"]))
            started = time.monotonic()

            async def send():
                for i in range(0, len(payload), 16 * 1024):
                    await join.send(payload[i:i + 16 * 1024])

            sender = asyncio.create_task(send())
            got = await recv_exactly(host, len(payload), timeout=15)
            await sender
            took = time.monotonic() - started
            assert got == payload
            # One burst of `rate` bytes free, the other 2 * rate at `rate` per second.
            assert 1.6 < took < 6, took

    run(go())


def test_closing_the_room_closes_its_tunnels(live):
    srv = live()
    room = create(srv)

    async def go():
        async with Control(srv, room) as ctl:
            t = await open_tunnel(srv, room, ctl)
            join = await connect(data_url(srv, t["tunnel_id"], "join", t["join_token"]))
            host = await connect(data_url(srv, t["tunnel_id"], "host", room["host_token"]))
            import httpx
            async with httpx.AsyncClient() as http:
                r = await http.delete(f"{srv.http}/api/rooms/{room['room_id']}",
                                      headers={"Authorization": "Bearer " + room["host_token"]})
                assert r.status_code == 200
            for ws in (join, host):
                await asyncio.wait_for(ws.wait_closed(), 5)
            assert (await ctl.expect("closed"))["reason"] == "host_closed"
            with pytest.raises(ConnectionClosed):
                await join.send(b"late")

    run(go())
