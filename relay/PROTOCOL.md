# D2R Rooms relay — wire protocol v1

A relay carries the bytes of one D2R TCP/IP game between a host and its joiners when the host's port cannot be reached
from outside. Both sides dial OUT to the relay over WebSocket, so neither needs a forwarded port. The relay only pipes
bytes: the game, D2RLoader's handshake and its plugin network all run inside them, unchanged.

Every relay speaks exactly this protocol: the Cloudflare Worker (`relay/cloudflare`) and the Python server
(`relay/python`) are interchangeable, and `relay/conformance.py` checks any running relay against this document.

```
 Host PC                                        Relay                                   Joiner PC
 D2R host listener 127.0.0.1:<game_port>        one room                                D2R "Join Game: 127.0.0.1"
   ^ tcp (one per joiner)                                                                  | tcp
 D2R Rooms plugin ==== host socket (all pipes) ==== room ==== join socket (one pipe) ==== D2R Rooms plugin
                                                                                        (listens on 127.0.0.1:<game_port>)
```

## 1. Endpoints

All on one origin (`https://relay.example` / `wss://relay.example`). `ws://` is fine for a relay on a LAN or for tests.

| Request | What |
|---|---|
| `GET /healthz` | `200 {"ok":true,"relay":1,"name":"…","status":"active","open":true,"usage":0.12,"pipes":7,"max_message":65536}`: the relay's status (§1.1). Clients use it to pick a relay and to show its state. |
| `GET /v1/rooms/{room}/host` + WebSocket upgrade | the host's socket for room `{room}` |
| `GET /v1/rooms/{room}/join` + WebSocket upgrade | one joiner's socket: one pipe |

`{room}` is `[A-Za-z0-9_-]{16,64}`, chosen by the host's client at random (at least 128 bits). The relay keeps no list
of rooms anyone can read.

### 1.1 Status

| `status` | `open` | New rooms (host sockets for an unclaimed room) | New pipes (joiners) | Running pipes |
|---|---|---|---|---|
| `active` | true | accepted | accepted | go on |
| `near_limit` | true | refused: `503 near_limit` | accepted (rooms already made can still fill up) | go on |
| `limit` | false | refused: `503 near_limit` | refused: `503 over_budget` | go on |

`usage` (optional) is how much of the relay's cap is used, 0..1: its money budget on Cloudflare, its room count on a
VPS. A relay turns `near_limit` at its own threshold (both servers default to 0.8) and `limit` at 1. The fourth state a
client shows, **not answering**, is its own: no `200` from `/healthz` within 5 s.

A host that already claimed a room may always reconnect to it, whatever the status.

### Request headers

| Header | On | What |
|---|---|---|
| `X-Relay-Key` | host | Only for a relay that is closed to the public (its operator set keys). Joiners never need it: the join key below already lets them into one room of an allowed host. |
| `X-Host-Secret` | host | `[A-Za-z0-9_-]{16,128}`, random, kept by the host's client. The first host socket claims the room with it; a later host socket must send the same secret. |
| `X-Join-Key-Hash` | host | 64 lower-case hex: SHA-256 of the join key (UTF-8). The relay keeps only this hash. |
| `X-Join-Key` | join | `[A-Za-z0-9_-]{16,128}`: the room's join key. The host gives it to its joiners through the board, after the room's password. |

### Refusals

Before the upgrade the relay answers a plain HTTP status with `{"error": "<code>"}`:

| Status | `error` | When |
|---|---|---|
| 400 | `bad_request` | a malformed room id or header |
| 401 | `relay_key` | the relay has keys and `X-Relay-Key` is missing or wrong (host only) |
| 403 | `bad_secret` | the room is claimed with another host secret |
| 403 | `bad_join_key` | the join key does not match the room |
| 404 | `no_room` | no host socket is connected for this room (join only) |
| 404 | `not_found` | any other path |
| 409 | `room_full` | the room already has `pipes` open pipes |
| 426 | `upgrade_required` | a room path without a WebSocket upgrade |
| 503 | `near_limit` | the relay takes no new rooms right now (host only, status `near_limit` or `limit`) |
| 503 | `over_budget` | the relay refuses new pipes right now (join only, status `limit`; running pipes go on) |

A client that gets `over_budget`, `no_room` or no answer falls back to the host's own address.

## 2. Messages

Text messages are JSON, binary messages are game bytes. A client ignores a text message with a `t` it does not know.

### Both sockets

- Right after the upgrade the relay sends a text `{"t":"hello","v":1,"pipes":7,"max_message":65536}`. On a join socket
  it adds `"pipe": <n>`.
- Keep-alive: a client sends the text `{"t":"ping"}` at least every 30 s while idle. The relay answers `{"t":"pong"}`
  (byte for byte these strings, so a relay can answer without parsing).

### The host socket: frames

Every binary message on the host socket is a frame:

```
byte 0     type   1 = OPEN, 2 = DATA, 3 = CLOSE
byte 1     pipe   1..255
byte 2..   payload
```

| Frame | Direction | Payload | Meaning |
|---|---|---|---|
| OPEN | relay → host | none | a joiner connected as pipe `n`. The host's client connects to its game at `127.0.0.1:<game_port>`. |
| DATA | both | game bytes | relay → host: bytes from joiner `n`; host → relay: bytes for joiner `n` |
| CLOSE | both | optional UTF-8 reason | relay → host: joiner `n` is gone; host → relay: close joiner `n` (its game connection ended) |

- The relay sends OPEN for a pipe before any DATA of it. A joiner may send bytes before the host's client has
  connected to its game: the host's client buffers them until the connection is up.
- DATA or CLOSE for a pipe that is not open is ignored (it may have just closed).
- Payload at most `max_message` bytes (65 536). A larger message closes the socket with code 1009.

### The join socket

- Binary messages are the game's bytes, unframed, both ways.
- The relay closes the join socket (code 1000) when the host sends CLOSE for its pipe; the reason, when given, is the
  close reason.
- When the join socket closes, the relay sends CLOSE for its pipe to the host.

## 3. Lifetimes

- **A room** exists while its host socket is open. The claim (the host secret's hash and the join key's hash) is kept
  for 24 h after the host socket closes, so the same host can come back; after that the room id is free.
- **A second host socket** with the right secret replaces the first: the old one is closed with code 4000 `replaced`,
  and its pipes with it.
- **The host socket closing** closes every join socket of the room with code 1001 `host_gone`.
- **Pipes** are numbered from 1, the lowest free number first; at most `pipes` (7: D2R has 8 players) are open at once.

## 4. Limits

- Per pipe and direction: at most 256 KiB/s averaged, with bursts up to 1 MiB. A pipe over that is closed with the
  reason `rate` (both ends are told). Game traffic is a few KB/s, so this only stops abuse.
- A relay may refuse new rooms (`near_limit`) or new pipes (`over_budget`) at any time; it never cuts a running pipe
  for its budget.

## 5. Versions

The path carries the version (`/v1/`). A client reads `relay` from `/healthz` and `v` from `hello`. New text messages
and new `hello` fields may be added within v1; anything that changes the frames goes to `/v2/`.
