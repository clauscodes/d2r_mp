"""Rooms and tunnels, all in memory, all on the one event loop of one process.

A room is alive while its host's control socket is open (plus `host_grace`
after it drops) and the host keeps sending `state`. A tunnel joins one joiner's
data socket to one host data socket; each end attaches once.
"""

import asyncio
import hashlib
import hmac
import secrets
import time
from collections import defaultdict, deque
from dataclasses import dataclass, field
from typing import Any, Optional

from .config import Settings
from .models import CreateRoom, HostState, Mod, Player

CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
CODE_LEN = 4

SIDES = ("host", "join")


def new_token() -> str:
    return secrets.token_urlsafe(24)


def same_token(a: str, b: str) -> bool:
    return hmac.compare_digest(a.encode(), b.encode())


class AddressHasher:
    """Source addresses are only ever kept as a keyed hash; the key changes daily."""

    def __init__(self) -> None:
        self._day = -1
        self._key = b""

    def __call__(self, addr: str) -> str:
        day = int(time.time() // 86400)
        if day != self._day:
            self._day, self._key = day, secrets.token_bytes(32)
        return hmac.new(self._key, addr.encode(), hashlib.sha256).hexdigest()[:24]


class TokenBucket:
    """Throttles one direction of a tunnel: waits instead of dropping, like TCP."""

    def __init__(self, rate: int, burst: int) -> None:
        self.rate = float(rate)
        self.burst = float(max(burst, rate))
        self.tokens = self.burst
        self.stamp = time.monotonic()

    async def take(self, n: int) -> None:
        while True:
            now = time.monotonic()
            self.tokens = min(self.burst, self.tokens + (now - self.stamp) * self.rate)
            self.stamp = now
            if self.tokens >= n:
                self.tokens -= n
                return
            await asyncio.sleep((n - self.tokens) / self.rate)


class SlidingCounter:
    """Events per key within a time window (room creations, wrong passwords)."""

    def __init__(self) -> None:
        self._events: dict[Any, deque[float]] = defaultdict(deque)

    def count(self, key: Any, window: float) -> int:
        q = self._events.get(key)
        if not q:
            return 0
        cutoff = time.monotonic() - window
        while q and q[0] < cutoff:
            q.popleft()
        if not q:
            del self._events[key]
            return 0
        return len(q)

    def add(self, key: Any) -> None:
        self._events[key].append(time.monotonic())

    def prune(self, window: float) -> None:
        for key in list(self._events):
            self.count(key, window)


@dataclass(eq=False)
class Tunnel:
    tunnel_id: str
    room: "Room" = field(repr=False)
    join_token: str
    mod: Mod
    player: Optional[Player]
    created: float = field(default_factory=time.monotonic)
    sockets: dict[str, Any] = field(default_factory=dict)
    both: asyncio.Event = field(default_factory=asyncio.Event)
    closed: bool = False

    def token_ok(self, side: str, token: str) -> bool:
        want = self.room.host_token if side == "host" else self.join_token
        return same_token(token, want)

    def attach(self, side: str, ws: Any) -> None:
        self.sockets[side] = ws
        if all(s in self.sockets for s in SIDES):
            self.both.set()

    def peer(self, side: str) -> Any:
        return self.sockets["join" if side == "host" else "host"]


@dataclass(eq=False)
class Room:
    room_id: str
    code: str
    host_token: str
    addr: str
    name: str
    mod: Mod
    difficulty: str
    mode: str
    act: Optional[int]
    max_players: int
    public: bool
    password_hash: Optional[bytes]
    password_salt: bytes
    host_player: Optional[Player]
    created: float = field(default_factory=time.monotonic)
    control: Any = None
    control_lock: asyncio.Lock = field(default_factory=asyncio.Lock)
    host_lost_at: Optional[float] = field(default_factory=time.monotonic)
    last_state: float = field(default_factory=time.monotonic)
    roster: list[Player] = field(default_factory=list)
    vr: Optional[bool] = None
    tunnels: dict[str, Tunnel] = field(default_factory=dict)
    closed: bool = False

    def password_ok(self, password: str) -> bool:
        if self.password_hash is None:
            return True
        return hmac.compare_digest(hash_password(password, self.password_salt), self.password_hash)

    def players(self) -> list[dict]:
        """The public player rows: host roster merged with what joiners reported.

        The host's own entry (`self`) carries the room's mod; other roster entries
        take the mod of the joiner who reported the same name; joiners missing from
        the roster (no plugin on the host) are added from their own report.
        """
        by_name: dict[str, dict] = {}
        joiner_mods = {}
        for t in self.tunnels.values():
            if t.player is not None:
                joiner_mods.setdefault(t.player.name, (t.mod, t.player))

        def row(p: Player, mod: Optional[Mod], host: bool) -> dict:
            return {
                "name": p.name,
                "class": p.char_class,
                "level": p.level,
                "act": p.act,
                "area": p.area,
                "vr": p.vr,
                "host": host,
                "mod": mod.model_dump() if mod else None,
            }

        roster = list(self.roster)
        if not any(p.self_ for p in roster) and self.host_player is not None:
            roster.insert(0, self.host_player.model_copy(update={"self_": True}))
        for p in roster:
            if p.name in by_name:
                continue
            if p.self_:
                r = row(p, self.mod, True)
                if r["vr"] is None:
                    r["vr"] = self.vr
            else:
                mod = joiner_mods.get(p.name, (None, None))[0]
                r = row(p, mod, False)
            by_name[p.name] = r
        for name, (mod, p) in joiner_mods.items():
            if name not in by_name:
                by_name[name] = row(p, mod, False)
        return list(by_name.values())

    def player_count(self) -> int:
        # Without any roster the host and each tunnel count as one player.
        return max(len(self.players()), 1 + len(self.tunnels))

    def public_view(self) -> dict:
        return {
            "room_id": self.room_id,
            "code": self.code,
            "name": self.name,
            "mod": self.mod.model_dump(),
            "difficulty": self.difficulty,
            "mode": self.mode,
            "act": self.act,
            "max_players": self.max_players,
            "players_count": self.player_count(),
            "locked": self.password_hash is not None,
            "host_online": self.control is not None,
            "players": self.players(),
        }

    async def send_host(self, msg: dict) -> None:
        async with self.control_lock:
            ws = self.control
            if ws is None:
                return
            try:
                await ws.send_json(msg)
            except Exception:
                pass

    def apply_state(self, state: HostState) -> None:
        self.last_state = time.monotonic()
        self.roster = state.players
        if state.difficulty:
            self.difficulty = state.difficulty
        if state.mode:
            self.mode = state.mode
        if state.act:
            self.act = state.act
        if state.vr is not None:
            self.vr = state.vr


def hash_password(password: str, salt: bytes) -> bytes:
    return hashlib.scrypt(password.encode(), salt=salt, n=2**12, r=8, p=1, dklen=32)


class LimitError(Exception):
    def __init__(self, status: int, error: str) -> None:
        super().__init__(error)
        self.status = status
        self.error = error


class Registry:
    def __init__(self, settings: Settings) -> None:
        self.s = settings
        self.rooms: dict[str, Room] = {}
        self.by_code: dict[str, Room] = {}
        self.tunnels: dict[str, Tunnel] = {}
        self.creates = SlidingCounter()
        self.join_fails = SlidingCounter()

    # --- rooms -------------------------------------------------------------

    def _new_code(self) -> str:
        for _ in range(1000):
            code = "".join(secrets.choice(CODE_ALPHABET) for _ in range(CODE_LEN))
            if code not in self.by_code:
                return code
        raise LimitError(503, "no_free_code")

    def create_room(self, req: CreateRoom, addr: str) -> Room:
        if len(self.rooms) >= self.s.max_rooms:
            raise LimitError(503, "server_full")
        if sum(1 for r in self.rooms.values() if r.addr == addr) >= self.s.max_rooms_per_addr:
            raise LimitError(429, "too_many_rooms")
        if self.creates.count(addr, 60.0) >= self.s.creates_per_minute:
            raise LimitError(429, "too_many_creates")
        self.creates.add(addr)
        salt = secrets.token_bytes(16)
        room = Room(
            room_id=secrets.token_urlsafe(9),
            code=self._new_code(),
            host_token=new_token(),
            addr=addr,
            name=req.name,
            mod=req.mod,
            difficulty=req.difficulty,
            mode=req.mode,
            act=req.act,
            max_players=req.max_players,
            public=req.public,
            password_hash=hash_password(req.password, salt) if req.password else None,
            password_salt=salt,
            host_player=req.player,
        )
        self.rooms[room.room_id] = room
        self.by_code[room.code] = room
        return room

    def find(self, room_id: Optional[str] = None, code: Optional[str] = None) -> Optional[Room]:
        if room_id:
            return self.rooms.get(room_id)
        if code:
            return self.by_code.get(code.strip().upper())
        return None

    def list_public(self, mod: Optional[str] = None, difficulty: Optional[str] = None) -> list[dict]:
        out = []
        for r in self.rooms.values():
            if not r.public or r.control is None:
                continue
            if mod and r.mod.id != mod:
                continue
            if difficulty and r.difficulty != difficulty:
                continue
            out.append(r.public_view())
        out.sort(key=lambda v: v["name"].lower())
        return out

    async def close_room(self, room: Room, reason: str = "closed") -> None:
        if room.closed:
            return
        room.closed = True
        self.rooms.pop(room.room_id, None)
        if self.by_code.get(room.code) is room:
            del self.by_code[room.code]
        for t in list(room.tunnels.values()):
            await self.close_tunnel(t, notify=False)
        ws, room.control = room.control, None
        if ws is not None:
            try:
                await ws.send_json({"type": "closed", "reason": reason})
                await ws.close(code=1000)
            except Exception:
                pass

    # --- tunnels -----------------------------------------------------------

    def open_tunnel(self, room: Room, mod: Mod, player: Optional[Player]) -> Tunnel:
        if room.control is None:
            raise LimitError(409, "host_offline")
        if len(room.tunnels) >= min(self.s.max_tunnels, room.max_players - 1):
            raise LimitError(409, "room_full")
        t = Tunnel(tunnel_id=secrets.token_urlsafe(9), room=room, join_token=new_token(), mod=mod, player=player)
        room.tunnels[t.tunnel_id] = t
        self.tunnels[t.tunnel_id] = t
        return t

    async def close_tunnel(self, t: Tunnel, notify: bool = True, code: int = 1000) -> None:
        if t.closed:
            return
        t.closed = True
        t.both.set()  # wake an end still waiting for its peer
        self.tunnels.pop(t.tunnel_id, None)
        t.room.tunnels.pop(t.tunnel_id, None)
        for ws in list(t.sockets.values()):
            try:
                await ws.close(code=code)
            except Exception:
                pass
        if notify:
            await t.room.send_host({"type": "left", "tunnel_id": t.tunnel_id})

    # --- sweeper -----------------------------------------------------------

    async def sweep(self) -> None:
        now = time.monotonic()
        for room in list(self.rooms.values()):
            if room.control is None and room.host_lost_at is not None and now - room.host_lost_at > self.s.host_grace:
                await self.close_room(room, "host_gone")
            elif room.control is not None and now - room.last_state > self.s.state_timeout:
                await self.close_room(room, "no_state")
        for t in list(self.tunnels.values()):
            if not t.both.is_set() and now - t.created > self.s.tunnel_connect_timeout:
                await self.close_tunnel(t)
        self.creates.prune(60.0)
        self.join_fails.prune(self.s.join_fail_window)

    async def sweep_forever(self) -> None:
        while True:
            await asyncio.sleep(self.s.sweep_interval)
            try:
                await self.sweep()
            except Exception:  # never let one bad room stop the sweeper
                pass
