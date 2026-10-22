"""Rooms API: create, find, list, join, limits, validation, lifetime."""

import asyncio
import time

import httpx

from rooms.registry import CODE_ALPHABET
from util import ROOM, Control, ajoin, alist, by_code, create, run


def test_create_gives_code_and_token(live):
    srv = live()
    room = create(srv)
    assert len(room["code"]) == 4 and set(room["code"]) <= set(CODE_ALPHABET)
    assert len(room["host_token"]) >= 30
    r = by_code(srv, room["code"].lower())  # codes are typed by hand: any case
    assert r.status_code == 200
    view = r.json()["room"]
    assert view["name"] == "Baal runs" and view["difficulty"] == "hell"
    assert view["mod"] == {"id": "d2r-vr", "version": "0.175.0"}
    assert view["host_online"] is False
    assert by_code(srv, "ZZZZ").status_code == 404 or room["code"] == "ZZZZ"


def test_names_are_cleaned(live):
    srv = live()
    room = create(srv, name="  Baal\x00‮   runs \n")
    assert by_code(srv, room["code"]).json()["room"]["name"] == "Baal runs"


def test_validation_errors_do_not_echo_input(live):
    srv = live()
    r = httpx.post(srv.http + "/api/rooms", json={**ROOM, "name": "\x01\x02", "password": "secret-pw"})
    assert r.status_code == 422
    assert r.json()["error"] == "invalid"
    assert "secret-pw" not in r.text
    r = httpx.post(srv.http + "/api/rooms", json={**ROOM, "mod": {"id": "Bad Mod!"}})
    assert r.status_code == 422
    r = httpx.post(srv.http + "/api/rooms", json={**ROOM, "max_players": 9})
    assert r.status_code == 422


def test_rooms_per_address_limit(live):
    srv = live(max_rooms_per_addr=3)
    for _ in range(3):
        create(srv)
    r = httpx.post(srv.http + "/api/rooms", json=ROOM)
    assert r.status_code == 429 and r.json()["error"] == "too_many_rooms"


def test_creates_per_minute_limit(live):
    srv = live(max_rooms_per_addr=10, creates_per_minute=2)
    create(srv)
    create(srv)
    r = httpx.post(srv.http + "/api/rooms", json=ROOM)
    assert r.status_code == 429 and r.json()["error"] == "too_many_creates"


def test_join_needs_host_online(live):
    srv = live()
    room = create(srv)
    r = run(ajoin(srv, room))
    assert r.status_code == 409 and r.json()["error"] == "host_offline"


def test_delete_needs_the_host_token(live):
    srv = live()
    room = create(srv)
    url = f"{srv.http}/api/rooms/{room['room_id']}"
    assert httpx.delete(url, headers={"Authorization": "Bearer nope"}).status_code == 403
    assert httpx.delete(url, headers={"Authorization": "Bearer " + room["host_token"]}).status_code == 200
    assert by_code(srv, room["code"]).status_code == 404


def test_room_without_host_expires(live):
    srv = live(host_grace=0.5)
    room = create(srv)
    time.sleep(1.2)
    assert by_code(srv, room["code"]).status_code == 404


def test_list_shows_online_public_rooms_only(live):
    srv = live()
    public = create(srv)
    private = create(srv, name="Friends only", public=False)
    offline = create(srv, name="Nobody home")

    async def go():
        async with Control(srv, public), Control(srv, private):
            rooms = await alist(srv)
            assert [r["room_id"] for r in rooms] == [public["room_id"]]
            text = str(rooms)
            assert public["host_token"] not in text and "addr" not in text
            # A private room is still found by its code.
            async with httpx.AsyncClient() as http:
                r = await http.get(f"{srv.http}/api/rooms/by-code/{private['code']}")
                assert r.status_code == 200
            assert await alist(srv, mod="d2r-pcvr") == []
            assert len(await alist(srv, mod="d2r-vr", difficulty="hell")) == 1
            assert await alist(srv, difficulty="normal") == []
            assert offline["room_id"] not in text  # no host socket: not listed

    run(go())


def test_password_and_attempt_limit(live):
    srv = live(join_fail_limit=3)
    room = create(srv, password="hunter2")

    async def go():
        async with Control(srv, room):
            r = await ajoin(srv, room, password="hunter2")
            assert r.status_code == 200
            for _ in range(3):
                r = await ajoin(srv, room, password="wrong")
                assert r.status_code == 403 and r.json()["error"] == "bad_password"
            r = await ajoin(srv, room, password="hunter2")
            assert r.status_code == 429 and r.json()["error"] == "too_many_attempts"

    run(go())
    view = by_code(srv, room["code"]).json()["room"]
    assert view["locked"] is True


def test_room_full(live):
    srv = live()
    room = create(srv, max_players=2)

    async def go():
        async with Control(srv, room) as ctl:
            assert (await ajoin(srv, room)).status_code == 200
            await ctl.expect("tunnel")
            r = await ajoin(srv, room)
            assert r.status_code == 409 and r.json()["error"] == "room_full"

    run(go())


def test_join_by_code_and_mod_mismatch_flag(live):
    srv = live()
    room = create(srv)

    async def go():
        async with Control(srv, room) as ctl:
            async with httpx.AsyncClient() as http:
                r = await http.post(srv.http + "/api/join",
                                    json={"code": room["code"].lower(), "mod": {"id": "d2r-pcvr"}})
                assert r.status_code == 200
                assert r.json()["mod_mismatch"] is True
                msg = await ctl.expect("tunnel")
                assert msg["tunnel_id"] == r.json()["tunnel_id"] and msg["mod"]["id"] == "d2r-pcvr"
                r = await http.post(srv.http + "/api/join",
                                    json={"code": room["code"], "mod": {"id": "d2r-vr"}})
                assert r.json()["mod_mismatch"] is False

    run(go())


def test_player_rows_merge_roster_and_joiners(live):
    srv = live()
    room = create(srv, player={"name": "Typed", "class": "paladin", "level": 80})

    async def go():
        async with Control(srv, room) as ctl:
            # No roster yet: the host's typed character shows, with the room's mod.
            rows = (await alist(srv))[0]["players"]
            assert rows == [{"name": "Typed", "class": "paladin", "level": 80, "act": None, "area": "",
                             "vr": None, "host": True, "mod": {"id": "d2r-vr", "version": "0.175.0"}}]

            # Bob joins and reports himself; Eve joins without a report.
            r = await ajoin(srv, room, player={"name": "Bob", "class": "sorceress", "level": 82})
            assert r.status_code == 200
            assert (await ajoin(srv, room, mod={"id": "none"})).status_code == 200

            # The host's plugin reports the real roster.
            await ctl.send({"type": "state", "vr": True, "act": 5, "players": [
                {"name": "Aria", "class": "paladin", "level": 85, "act": 5, "area": "Worldstone Keep Level 2",
                 "self": True},
                {"name": "Bob", "class": "sorceress", "level": 82, "act": 5, "area": "Throne of Destruction"},
                {"name": "Eve", "class": "druid", "level": 79, "act": 4, "area": "River of Flame"},
            ]})
            await ctl.send({"type": "ping"})
            await ctl.expect("pong")
            view = (await alist(srv))[0]
            rows = {p["name"]: p for p in view["players"]}
            assert set(rows) == {"Aria", "Bob", "Eve"}
            assert rows["Aria"]["host"] and rows["Aria"]["vr"] is True
            assert rows["Aria"]["mod"]["id"] == "d2r-vr" and rows["Aria"]["act"] == 5
            assert rows["Bob"]["mod"]["id"] == "d2r-pcvr" and rows["Bob"]["area"] == "Throne of Destruction"
            assert rows["Eve"]["mod"] is None  # she joined without naming her character: no match
            assert view["players_count"] == 3 and view["act"] == 5

            await ctl.send({"type": "state", "players": [{"name": "", "class": "paladin"}]})
            assert (await ctl.expect("error"))["error"] == "bad_state"
            await ctl.send({"type": "what"})
            assert (await ctl.expect("error"))["error"] == "unknown_message"

    run(go())


def test_room_without_state_is_removed(live):
    srv = live(state_timeout=0.8)
    room = create(srv)

    async def go():
        async with Control(srv, room) as ctl:
            msg = await ctl.expect("closed", timeout=5)
            assert msg["reason"] == "no_state"
        assert by_code(srv, room["code"]).status_code == 404

    run(go())


def test_host_reconnect_keeps_room_and_pending_tunnels(live):
    srv = live(host_grace=3.0, tunnel_connect_timeout=10.0)
    room = create(srv)

    async def go():
        async with Control(srv, room):
            r = await ajoin(srv, room)
            tid = r.json()["tunnel_id"]
        await asyncio.sleep(0.3)
        assert by_code(srv, room["code"]).json()["room"]["host_online"] is False
        async with Control(srv, room) as ctl:
            assert ctl.hello["tunnels"] == [tid]

    run(go())


def test_second_control_socket_replaces_the_first(live):
    srv = live()
    room = create(srv)

    async def go():
        async with Control(srv, room) as first:
            async with Control(srv, room):
                await asyncio.wait_for(first.ws.wait_closed(), 5)
                assert first.ws.close_code == 4000
                assert by_code(srv, room["code"]).json()["room"]["host_online"] is True

    run(go())
