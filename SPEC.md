# D2R Rooms — SPEC (draft v0.1, 2026-10-22)

A small helper that lets D2R players (modded through D2RLoader) play together
over the game's own **TCP/IP** multiplayer without port forwarding or sharing
IP addresses: a room list, quick join by code, and a relay.

Status: server, reference helper (`tools/helper.py`) and tests built; launcher
tab and roster block not built.

## 1. Goals and non-goals

Goals
- A player creates a **room**; others see it in a **list** (who plays, which
  mod, class, level, act, difficulty) or join by a **short code**.
- Works for **both VR mods** — D2R VR Authentic (`mods\D2R-VR`, mod id `d2r-vr`)
  and D2R VR Unleashed (`mods\D2R-PCVR`, mod id `d2r-pcvr`) — and for a game
  without a VR mod. Every player is labelled with the mod they play.
- No router setup: both sides only make **outgoing** connections (HTTPS/WSS 443).
- No player IP address is ever shown to other players or kept on disk.

Non-goals
- Never touches Battle.net or the game's own servers; never changes game data,
  saves or game rules. The game runs its own TCP/IP host/join exactly as with a
  LAN or VPN; the relay only carries those bytes.
- No accounts, no chat, no matchmaking by skill.

## 2. Independence

D2R Rooms is an independent project, not part of any mod:
- its own repository, its own licence (MIT);
- its own server on its own hosting and domain (placeholder below:
  `rooms.example.net`); no other project's domain, key or API in the code;
- the server address is configuration (`server_url`), not compiled in, so a
  community can run its own server.

The mods and the launcher are **clients** of a documented protocol (§5), the
same way any client could be.

## 3. How a connection runs

```
 Host PC                                   Server                        Joiner PC
 D2R "Host game" ← tcp 127.0.0.1:4000 ←  helper ⇄ WSS ⇄ relay ⇄ WSS ⇄ helper ← tcp 127.0.0.1:4000 ← D2R "Join: 127.0.0.1"
```

1. **Host** clicks *Create room* in the helper (the launcher tab, §6). The helper
   registers the room (`POST /api/rooms`) and keeps a control WebSocket open.
   The player hosts a TCP/IP game in D2R as usual.
2. **Joiner** picks the room in the list or types its code. The helper asks for
   a tunnel (`POST /api/join`), opens its data WebSocket and starts listening on
   `127.0.0.1:4000`. In D2R the joiner joins TCP/IP game `127.0.0.1`.
3. The server tells the host's helper about the new tunnel on the control
   socket; the host's helper opens its own data WebSocket for that tunnel and a
   TCP connection to its local game at `127.0.0.1:4000`.
4. The relay pipes binary frames between the two data sockets of the tunnel.
   Either side closing closes the other.

One tunnel per joiner (D2R allows up to 8 players → at most 7 tunnels per room).

To verify before building (§9): the game's TCP/IP port and whether it can be
changed; whether the joiner's game itself needs a local port that clashes.

## 4. What the room list shows

Room: name, difficulty, Softcore/Hardcore, players `n/8`, password lock,
host's mod + version, server ping.

Player row: **character name, class, level, act + area**, mod label
(`Authentic 0.175` / `Unleashed 0.3.19` / `no VR mod`), VR or flat.

Where the data comes from:
- **Host roster (preferred).** In a TCP/IP game the host's game knows every
  player in it. A plugin in the host's game writes the roster to the
  *roster block* (§7); the host's helper sends it on the control socket every
  5 s. So one plugin on the host is enough for the whole list.
- **Self report.** Each joiner's helper reports its own mod/version (it knows
  which mod it launched) and, if its plugin writes the block, its own character.
  Merged with the host roster by character name.
- **Nothing.** A room hosted without a plugin shows what the host typed when
  creating it (difficulty, act) and the joiners' mod labels only.

Mixed mods in one game: the list shows each player's mod; joining a room whose
host plays another mod shows a warning, not a block (to verify that Authentic,
Unleashed and no-mod players really play together, §9).

## 5. Server protocol (v1)

All JSON, `"v": 1`. HTTPS + WSS on 443 behind a reverse proxy.

| Call | Who | Body / answer |
|---|---|---|
| `POST /api/rooms` | host | `{name, mod:{id,version}, difficulty, mode, max_players, public, password?}` → `{room_id, code, host_token}` |
| `GET /api/rooms?mod=&difficulty=` | anyone | public rooms with players (§4); never IPs, never tokens |
| `GET /api/rooms/by-code/{code}` | anyone | one room (public or not) |
| `DELETE /api/rooms/{room_id}` | host | `Authorization: Bearer <host_token>` → closes the room and its tunnels |
| `POST /api/join` | joiner | `{room_id | code, password?, mod:{id,version}, player?}` → `{tunnel_id, join_token, mod_mismatch}` |
| `WS /ws/host/{room_id}?token=` | host | control: server→`hello` (with tunnels still waiting for the host), `tunnel`, `left`, `closed`, `error`, `pong`; host→`state` every 5 s (`players`, `difficulty`, `mode`, `act`, `vr`), `kick {tunnel_id}`, `ping`. A second control socket replaces the first (close 4000) |
| `WS /ws/data/{tunnel_id}?side=host|join&token=` | both | binary frames = raw TCP bytes, in order |

Room lifetime: lives while the host's control socket is open, 20 s grace for a
reconnect; no state message for 30 s removes it.

Codes: 4 characters from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`, unique among live rooms.

Limits (configurable): 3 rooms per source address, 7 tunnels per room,
frame ≤ 64 KiB, ≤ 128 KiB/s per tunnel, room/char names ≤ 32 printable chars,
join attempts with a wrong password rate-limited.

Storage: rooms in memory only. Logs keep no IP addresses (hashed with a daily
salt when needed for rate limits).

## 6. The helper (launcher tab)

Lives in the VR mods' launcher (Win32 C++) as a **Multiplayer** tab; WinHTTP,
which the launcher already uses, supports WebSockets — no new dependency.
Contents:
- room list with filters (my mod / same act / my level ±10 / VR only), *Join*;
- *Join by code*; *Create room* (name, difficulty, mode, password, public);
- status line: "Tunnel up — in D2R join TCP/IP game 127.0.0.1", ping, bytes.

The tunnel must keep running while the game runs, so the launcher stays alive
(minimised / tray) while a room or tunnel is open.

Settings: `server_url` (default: the project's server), *show my character
in the public list* (on), *public room* default.

Could later be a tiny standalone exe for players without the launcher; the
protocol does not change.

## 7. Roster block (game → helper)

A named shared-memory block `Local\D2RRooms.Roster.v1`, written by a plugin
inside the game, read by the helper. UTF-8 JSON, length-prefixed, a sequence
number to detect a torn read:

```json
{ "v":1, "seq":42, "mod":{"id":"d2r-vr","version":"0.175.0"}, "vr":true,
  "game":{"host":true, "difficulty":"hell", "mode":"softcore"},
  "players":[ {"name":"Aria","class":"paladin","level":85,"act":5,"area":"Worldstone Keep Level 2","self":true} ] }
```

- Read-only from the game: the plugin only **reads** units and party info;
  it changes nothing in the game.
- Any mod's plugin can write it (Authentic first, Unleashed welcome); the
  contract is this section.
- No block → the helper falls back to §4 "self report / nothing".

## 8. Server deploy

- One container (Python 3.12, FastAPI + uvicorn websockets, no database),
  `docker compose up -d`, listens on `127.0.0.1:8080`.
- TLS by Caddy (automatic Let's Encrypt) in the same compose file; only 443/80 open.
- A cheap VPS near the players (EU first); a second region later = a second
  `server_url`, the helper picks by ping.
- `deploy.sh` like: pull, build, restart; `/healthz` for a check.

## 9. To verify before building

1. D2R TCP/IP port (believed 4000/tcp) and whether it can be changed.
2. Authentic + Unleashed + no-mod players in one TCP/IP game (LAN games so
   far were one mod per game).
3. Does a joining client see the other players' act/area and level in the
   party data, or only the host? (Decides how much the host roster gives.)
4. Relay latency acceptable for melee in VR (measure ping via the relay vs LAN).
5. Can the plugin fill D2R's "join TCP/IP game" address field, or does the
   player type `127.0.0.1` once (the game remembers it)?

## 10. Plan

1. Server: rooms API + relay + limits, tests (pipe bytes through a fake tunnel).
2. Helper core in C++: local listener ↔ WSS tunnel, tested against the server
   with two fake "games" (echo sockets).
3. A real LAN-free test: two PCs, one D2R each, through the server.
4. Launcher Multiplayer tab.
5. Roster block in the Authentic plugin; the same contract offered to Unleashed.
