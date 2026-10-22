"""End to end through the reference helper: fake game <-> host helper <-> relay <-> join helper <-> fake client.

The "game" is an echo server that answers each line with its upper-case copy,
which is all the relay needs to prove: bytes from a local TCP client reach the
host's local game and the answer comes back, for several clients at once.
"""

import asyncio
import json

import httpx
import pytest

from helper import Host, Joiner
from util import alist, run


async def fake_game():
    async def serve(reader, writer):
        while line := await reader.readline():
            writer.write(line.upper())
            await writer.drain()
        writer.close()

    server = await asyncio.start_server(serve, "127.0.0.1", 0)
    return server, server.sockets[0].getsockname()[1]


def test_two_clients_through_both_helpers(live, tmp_path):
    srv = live()
    roster = tmp_path / "roster.json"
    roster.write_text(json.dumps({"act": 2, "players": [
        {"name": "Aria", "class": "paladin", "level": 30, "act": 2, "area": "Lut Gholein", "self": True}]}))

    async def go():
        game, game_port = await fake_game()
        room = {"name": "Duriel", "mod": {"id": "d2r-vr", "version": "0.175.0"}}
        host = Host(srv.http, room, ("127.0.0.1", game_port), roster)
        async with httpx.AsyncClient() as http:
            await host.create(http)
            control = asyncio.create_task(host.control())
            await asyncio.wait_for(host.ready.wait(), 5)

            joiner = Joiner(srv.http, {"code": host.code, "mod": {"id": "d2r-pcvr", "version": "0.3.19"},
                                       "player": {"name": "Bob", "class": "sorceress", "level": 29}},
                            ("127.0.0.1", 0))
            port = await joiner.start(http)

            async def client(tag: str):
                reader, writer = await asyncio.open_connection("127.0.0.1", port)
                for i in range(50):
                    writer.write(f"{tag} line {i}\n".encode())
                    await writer.drain()
                    assert await reader.readline() == f"{tag} LINE {i}\n".upper().encode()
                writer.close()

            await asyncio.gather(client("alpha"), client("beta"))

            # While a tunnel of Bob's is up, the list shows him with his own mod.
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.write(b"ping\n")
            assert await reader.readline() == b"PING\n"
            rows = {p["name"]: p for p in (await alist(srv))[0]["players"]}
            assert rows["Aria"]["area"] == "Lut Gholein" and rows["Aria"]["mod"]["id"] == "d2r-vr"
            assert rows["Bob"]["mod"]["id"] == "d2r-pcvr" and rows["Bob"]["host"] is False
            writer.close()

            joiner.server_obj.close()
            control.cancel()
            await host.delete(http)
            game.close()

    run(go())


def test_joiner_learns_quickly_when_the_host_game_is_down(live):
    srv = live(tunnel_connect_timeout=10.0)

    async def go():
        room = {"name": "Nobody hosts", "mod": {"id": "none"}}
        host = Host(srv.http, room, ("127.0.0.1", 1))  # nothing listens on port 1
        async with httpx.AsyncClient() as http:
            await host.create(http)
            control = asyncio.create_task(host.control())
            await asyncio.wait_for(host.ready.wait(), 5)
            joiner = Joiner(srv.http, {"room_id": host.room_id, "mod": {"id": "none"}}, ("127.0.0.1", 0))
            port = await joiner.start(http)
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.write(b"hello\n")
            # Closed long before the 10 s tunnel timeout.
            assert await asyncio.wait_for(reader.read(), 5) == b""
            writer.close()
            joiner.server_obj.close()
            control.cancel()

    run(go())


def test_joiner_sees_refusal(live, capsys):
    srv = live()

    async def go():
        joiner = Joiner(srv.http, {"code": "QQQQ", "mod": {"id": "none"}}, ("127.0.0.1", 0))
        async with httpx.AsyncClient() as http:
            port = await joiner.start(http)
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            assert await asyncio.wait_for(reader.read(), 5) == b""
            writer.close()
            joiner.server_obj.close()

    run(go())
    assert "join refused: 404" in capsys.readouterr().out


@pytest.mark.parametrize("server, want", [("https://rooms.example.net/", "wss://rooms.example.net"),
                                          ("http://127.0.0.1:8080/base", "ws://127.0.0.1:8080/base")])
def test_ws_base(server, want):
    from helper import ws_base
    assert ws_base(server) == want
