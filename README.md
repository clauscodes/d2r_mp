# D2R Rooms

Room list, quick join and relay for Diablo II: Resurrected TCP/IP games —
play together without port forwarding. Works with the D2R VR mods
(Authentic, Unleashed) and without them.

Design: [SPEC.md](SPEC.md).

## What is here

| Path | What |
|---|---|
| `server/rooms/` | the server: rooms API, host control socket, byte relay, limits |
| `tools/helper.py` | reference helper (host / join / list) — the client side in plain Python |
| `tests/` | API, relay (fake tunnel of raw sockets) and end-to-end through the helper |
| `docker-compose.yml`, `Caddyfile`, `deploy.sh` | the server with automatic HTTPS |

## Develop

```
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements-dev.txt
.venv/Scripts/python -m pytest
```

Run locally: `cd server && uvicorn rooms.app:app --port 8080 --ws websockets-sansio --no-access-log --log-level warning`.

## Try with real games (before the launcher tab exists)

Host PC (D2R hosts a TCP/IP game):

```
python tools/helper.py host --server https://rooms.example.net --name "Baal runs" --mod d2r-vr --version 0.175.0
```

It prints the room code. Joiner PC:

```
python tools/helper.py join --server https://rooms.example.net --code K7QX --mod d2r-pcvr
```

then in D2R join the TCP/IP game `127.0.0.1`. `python tools/helper.py list --server …` shows the rooms.

## Deploy

On a VPS with Docker, a DNS name pointing to it and ports 80/443 open:

```
git clone <this repo> d2r_rooms && cd d2r_rooms
cp .env.example .env    # set ROOMS_DOMAIN
./deploy.sh
```

One process holds all rooms in memory: keep a single `rooms` container and
worker. No access logs are written, by the app or by Caddy.
