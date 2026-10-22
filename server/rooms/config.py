"""Limits and timings. Every value can be set from the environment (ROOMS_<NAME>)."""

import os
from dataclasses import dataclass, fields


@dataclass
class Settings:
    # Live rooms one source address may host at once, and the whole server.
    max_rooms_per_addr: int = 3
    max_rooms: int = 500
    # Room creations per source address per minute (a host that crashes and
    # recreates stays well under this).
    creates_per_minute: int = 10
    # D2R holds 8 players, so a room has at most 7 joiners; one tunnel each.
    max_tunnels: int = 7
    # Largest relayed frame and the per-tunnel, per-direction byte rate.
    frame_max: int = 64 * 1024
    tunnel_rate: int = 128 * 1024
    # A new room waits this long for its host's control socket, and a host that
    # drops keeps the room this long for a reconnect.
    host_grace: float = 20.0
    # No state message from the host for this long removes the room.
    state_timeout: float = 30.0
    # Both ends of a tunnel must be attached within this time.
    tunnel_connect_timeout: float = 15.0
    # Wrong passwords per source address and room before 429, and the window.
    join_fail_limit: int = 5
    join_fail_window: float = 60.0
    # How often the sweeper looks for dead rooms and tunnels.
    sweep_interval: float = 5.0
    # Behind Caddy: take the client address from the right end of
    # X-Forwarded-For. Off when the server is reached directly.
    trust_proxy: bool = False

    @classmethod
    def from_env(cls) -> "Settings":
        s = cls()
        for f in fields(cls):
            raw = os.environ.get("ROOMS_" + f.name.upper())
            if raw is None:
                continue
            if f.type in (bool, "bool"):
                setattr(s, f.name, raw.strip().lower() in ("1", "true", "yes", "on"))
            elif f.type in (int, "int"):
                setattr(s, f.name, int(raw))
            else:
                setattr(s, f.name, float(raw))
        return s
