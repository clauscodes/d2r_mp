# D2R Rooms — SPEC (v0.2, 2026-10-22)

A board of open Diablo II: Resurrected **TCP/IP** games. A host publishes a
room; other players see it in a list (who plays, class, level, act, which mod)
or find it by a short code, get the host's address and connect their game
**directly** to it. No game traffic passes through the board.

Status: built — Cloudflare Worker + D1 (`worker/`), web page, reference helper
(`tools/helper.py`), tests. Not built: the launcher tab, the roster block in
the mods' plugins, UPnP port opening.

## 1. Goals and non-goals

Goals
- Create a room, list open rooms, join by list or by code.
- Works for **both VR mods** — D2R VR Authentic (mod id `d2r-vr`) and D2R VR
  Unleashed (`d2r-pcvr`) — and for a game without a VR mod (`none`). Every
  player is labelled with the mod they play.
- Runs on a **free** platform with no server to look after.

Non-goals
- Never touches Battle.net or the game's own servers, game data, saves or rules.
  The game runs its own TCP/IP host/join exactly as on a LAN.
- No relay of game traffic, no accounts, no chat.

## 2. Independence

D2R Rooms is an independent project, not part of any mod: its own repository,
its own licence (MIT), its own Cloudflare account and address. The mods and the
launcher are clients of the protocol in §5, like any other client.

## 3. How a game is found

1. The **host** creates a room — from the web page or the helper/launcher. The
   board takes the host's address from the connection (`CF-Connecting-IP`), or
   the host gives one (a VPN address such as Radmin VPN / ZeroTier, or a name).
   The host hosts a TCP/IP game in D2R as usual.
2. The host's client sends a **heartbeat** every 60 s (with the roster, §4).
   A room without a heartbeat for 180 s disappears from every answer.
3. A **player** picks the room or types its code and presses Join. The board
   answers with the host's address; in D2R: Play → TCP/IP → Join Game → that
   address.

The host must be reachable: an open port (D2R TCP/IP, believed 4000/tcp), or
everyone in one VPN. The board does not change that. A later helper step can
open the port by UPnP.

Addresses: the public list never shows the host's address; Join hands it out
(after the password when the room has one). D2R TCP/IP is IPv4, so the helper
talks to the board over IPv4; an IPv6 address is flagged (`address_ipv6`) and
the page tells the host to enter an IPv4/VPN address.

## 4. What the list shows

Room: name, code, difficulty, Softcore/Hardcore, act, players `n/max`, lock,
the host's mod + version.

Player row: **name, class, level, act + area**, mod label, VR or not.

Where it comes from:
- **Host roster** (preferred): a plugin in the host's game sees everyone in the
  game and writes the roster block (§7); the host's client sends it with the
  heartbeat. The roster is then the truth: the host's own entry (`self`) gets
  the room's mod, the others the mod of the joiner who gave the same name.
- **Without a roster**: the character the host typed when creating the room,
  and joiners who named their character when joining (shown 30 min).

Mixed mods: the list shows each player's mod; Join answers `mod_mismatch` when
the player's mod differs from the host's (a warning, not a block).

## 5. API (v1)

JSON, every answer carries `"v": 1`; errors are `{v, error, detail?}`.

| Call | Body / answer |
|---|---|
| `GET /` | the web page |
| `GET /api/rooms?mod=&difficulty=` | `{rooms: [view]}` — public live rooms; never addresses or tokens |
| `GET /api/rooms/by-code/{code}` | `{room: view}` — public or not; any letter case |
| `POST /api/rooms` | `{name, mod:{id,version}, difficulty?, mode?, act?, max_players?, public?, password?, address?, port?, player?}` → `{room_id, code, host_token, address, port, address_ipv6, heartbeat_every, ttl}` |
| `POST /api/rooms/{id}/heartbeat` | `Authorization: Bearer <host_token>`; `{players?, difficulty?, mode?, act?, vr?}` → `{ok, heartbeat_every}`; 404 when the room expired (create it again) |
| `DELETE /api/rooms/{id}` | `Authorization: Bearer <host_token>` |
| `POST /api/join` | `{room_id \| code, password?, mod?, player?}` → `{room_id, address, port, address_ipv6, mod_mismatch}` |

`view` = `{room_id, code, name, mod, difficulty, mode, act, max_players,
players_count, locked, age, players: [{name, class, level, act, area, vr, host, mod}]}`.

Codes: 4 characters from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`, unique among stored rooms.

Limits (Worker vars): 3 live rooms per address, 10 creations per address per
minute, 500 rooms, 5 wrong passwords per address and room per minute; names
≤ 32 printable characters (control and format characters removed).

## 6. Storage and privacy

One D1 (SQLite) database, two tables: `rooms` (one row per room; roster and
joiners as JSON in `data`) and `attempts` (creations and wrong passwords for the
limits). Expired rooms and attempts older than an hour are deleted on the next
room creation.

Kept: the host's address while the room lives (it is what Join hands out).
Not kept: tokens (SHA-256 only), passwords (PBKDF2), other players' addresses
(only a keyed hash for the limits, key `ADDR_SALT` + the day). Worker logs and
observability are off.

Free plan fit: a heartbeat writes one row; at one per minute 100 000 writes a
day cover about 1 600 room-hours.

## 7. Roster block (game → helper)

A named shared-memory block `Local\D2RRooms.Roster.v1`, written by a plugin
inside the game, read by the helper/launcher. UTF-8 JSON, length-prefixed, a
sequence number to detect a torn read:

```json
{ "v":1, "seq":42, "mod":{"id":"d2r-vr","version":"0.175.0"}, "vr":true,
  "game":{"host":true, "difficulty":"hell", "mode":"softcore"},
  "players":[ {"name":"Aria","class":"paladin","level":85,"act":5,"area":"Worldstone Keep Level 2","self":true} ] }
```

The plugin only **reads** units and party info; it changes nothing in the game.
Any mod's plugin can write it (Authentic first, Unleashed welcome). No block →
§4 "without a roster".

## 8. Clients

- **Web page** (`GET /`): list with filters, Join (shows the address with a
  Copy button), Create room (the page keeps the heartbeat while it is open).
- **Helper** (`tools/helper.py`): `list`, `host` (heartbeat with `--roster`,
  removes the room on Ctrl+C), `join` (prints the address).
- **Launcher tab** (not built): the helper's behaviour in the VR mods' launcher,
  reading the roster block, later opening the port by UPnP.

## 9. To verify

1. D2R TCP/IP port (believed 4000/tcp) and whether it can be changed.
2. Authentic + Unleashed + no-mod players in one TCP/IP game.
3. Does a joining client see the other players' act/area and level in the
   party data, or only the host?
4. Whether D2R accepts an IPv6 address or a host name in Join Game.
