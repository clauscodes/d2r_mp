"""Reference helper: the board's client side in plain Python.

It does what the launcher's Multiplayer tab will do:

    python helper.py list --server https://rooms.example.net
    python helper.py host --server URL --name "Baal runs" --mod d2r-vr --version 0.175.0
    python helper.py join --server URL --code K7QX --mod d2r-pcvr

host: publishes the room and keeps it alive (a heartbeat every minute, with the
roster from --roster when given) until Ctrl+C, which removes it. D2R hosts the
TCP/IP game as usual.
join: prints the host's address; in D2R choose TCP/IP, Join Game, and enter it.
No game traffic goes through the board.
"""

import argparse
import asyncio
import contextlib
import json
import sys
from pathlib import Path
from typing import Optional

import httpx


def log(*args) -> None:
    print(*args, flush=True)


def client(timeout: float = 10) -> httpx.AsyncClient:
    # IPv4 on purpose: the board takes the host's address from the connection,
    # and a D2R TCP/IP join needs an IPv4 address.
    return httpx.AsyncClient(timeout=timeout, transport=httpx.AsyncHTTPTransport(local_address="0.0.0.0", retries=2))


def address_text(a: dict) -> str:
    if a.get("port", 4000) == 4000:
        return a["address"]
    host = f"[{a['address']}]" if ":" in a["address"] else a["address"]
    return f"{host}:{a['port']}"


class Host:
    def __init__(self, server: str, room: dict, roster: Optional[Path] = None):
        self.server = server.rstrip("/")
        self.room = room  # the create request body
        self.roster = roster
        self.info: dict = {}

    @property
    def auth(self) -> dict:
        return {"Authorization": "Bearer " + self.info["host_token"]}

    async def create(self, http: httpx.AsyncClient) -> dict:
        r = await http.post(self.server + "/api/rooms", json=self.room)
        if r.status_code != 200:
            raise SystemExit(f"room not created: {r.status_code} {r.text}")
        self.info = r.json()
        log(f"room {self.info['code']} - players connect to {address_text(self.info)}")
        if self.info.get("address_ipv6"):
            log("note: your address is IPv6; give --address with your IPv4 or VPN address if players cannot connect")
        if self.state():
            await self.heartbeat(http)  # the roster now, not a minute later
        return self.info

    def state(self) -> dict:
        if not (self.roster and self.roster.exists()):
            return {}
        with contextlib.suppress(Exception):
            data = json.loads(self.roster.read_text(encoding="utf-8"))
            return {k: data[k] for k in ("players", "difficulty", "mode", "act", "vr") if k in data}
        return {}

    async def heartbeat(self, http: httpx.AsyncClient) -> bool:
        """One heartbeat; False when the room is gone (expired or removed)."""
        r = await http.post(f"{self.server}/api/rooms/{self.info['room_id']}/heartbeat", json=self.state(), headers=self.auth)
        return r.status_code != 404

    async def keep_alive(self, http: httpx.AsyncClient) -> None:
        every = self.info.get("heartbeat_every", 60)
        while True:
            await asyncio.sleep(every)
            try:
                if not await self.heartbeat(http):
                    log("the room expired on the board; creating it again")
                    await self.create(http)
            except httpx.HTTPError as e:
                log(f"heartbeat failed ({e}); retrying")

    async def delete(self, http: httpx.AsyncClient) -> None:
        with contextlib.suppress(Exception):
            await http.delete(f"{self.server}/api/rooms/{self.info['room_id']}", headers=self.auth)


async def join(server: str, target: dict, http: httpx.AsyncClient) -> Optional[dict]:
    r = await http.post(server.rstrip("/") + "/api/join", json=target)
    if r.status_code != 200:
        log(f"join refused: {r.status_code} {r.json().get('error', r.text)}")
        return None
    return r.json()


async def cmd_list(args) -> None:
    async with client() as http:
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
            "password": args.password, "port": args.port}
    if args.address:
        room["address"] = args.address
    host = Host(args.server, room, Path(args.roster) if args.roster else None)
    async with client() as http:
        await host.create(http)
        try:
            await host.keep_alive(http)
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
    async with client() as http:
        a = await join(args.server, target, http)
    if a:
        log(address_text(a))
        if a.get("mod_mismatch"):
            log("note: the host plays another mod")
        log("in D2R: Play -> TCP/IP -> Join Game -> this address")


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
    h.add_argument("--address", help="address players connect to (default: what the board sees, e.g. your VPN IP)")
    h.add_argument("--port", type=int, default=4000)
    h.add_argument("--roster", help="JSON file with players/difficulty/act, sent with every heartbeat")

    j = common(sub.add_parser("join"))
    g = j.add_mutually_exclusive_group(required=True)
    g.add_argument("--code")
    g.add_argument("--room")
    j.add_argument("--mod", default="none")
    j.add_argument("--version", default="")
    j.add_argument("--password", default="")
    j.add_argument("--char", help="your character name, shown in the room list")
    j.add_argument("--char-class", default="unknown")
    j.add_argument("--level", type=int)

    args = p.parse_args(argv)
    run = {"list": cmd_list, "host": cmd_host, "join": cmd_join}[args.cmd]
    with contextlib.suppress(KeyboardInterrupt):
        asyncio.run(run(args))


if __name__ == "__main__":
    sys.exit(main())
