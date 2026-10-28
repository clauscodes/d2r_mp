# D2R Rooms — D2RLoader plugin

The room board inside the game. Open **TCP/IP** in D2R's multiplayer menu and
the rooms list opens beside it:

- **Join** — asks the board for the host's address, opens D2R's own *Join
  Game* box, types the address and presses OK. For a room with a password,
  type the password into the field at the bottom first.
- **Create** — lists a room (the field at the bottom is its name) and goes on
  like D2R's own *Host Game* button. The room stays listed while you play and
  is removed when you leave the game, or with *Close room*.

Works with any D2RLoader setup — D2R VR Authentic, D2R VR Unleashed or no mod.
It is a client-only plugin: nothing in game data or saves, not needed by other
players in the game.

## Install

Copy `d2rl-d2r-rooms.dll` into `<game>/d2rloader/plugins/`. Needs D2RLoader
1.3.1 or newer (plugin ABI 4, SDK 0.3.0 services: Panel, Resource,
SharedEvent, Widget, Thread, Http, Lifecycle). Players who host or join through
a relay all need it; others do not.

The board address defaults to `https://d2r-rooms.d2r-rooms.workers.dev`; the
environment variable `D2R_ROOMS_SERVER` points it elsewhere.

## Relay (0.2)

When D2RLoader reports the hosted port **unreachable from outside** (its own
UPnP / PCP / NAT-PMP could not open the router), the room goes through a relay
([relay/PROTOCOL.md](../relay/PROTOCOL.md)):

- **Host:** after Create, the plugin reads `d2rloader/logs/d2rloader.log` for
  D2RLoader's verdict. "Unreachable" (or a failed router mapping with no
  "reachable" 20 s later) picks the first relay of the list whose `/healthz`
  says `active`, claims a room there and sends `{url, room, key}` to the board
  in a heartbeat. Each joiner arriving there becomes a TCP connection to the
  game on `127.0.0.1:<game_port>`.
- **Joiner:** a room with a relay opens the relay socket first, then a listener
  on `127.0.0.1:<game_port>` (127.0.0.2 / .3 when taken), and types that address
  into Join Game. If the relay refuses or does not answer, the host's address is
  tried as before.
- **The list:** `d2rloader/plugins/d2r-rooms.ini`, written with ours on the
  first start. `mode = auto | always | never`; one `name = wss://address [key]`
  line per relay. Guide: [docs/host_a_relay.md](../docs/host_a_relay.md).
- **Status:** the panel's status line shows every listed relay (active / near
  its limit / limit reached / not answering), and each room that uses a relay
  shows its relay's state, from the relay's `/healthz` (asked at most once a
  minute).
- `game_port` is read from `d2rloader/config/d2rloader.toml` before each host or
  join.

All network work runs on the plugin's own threads (WinHTTP WebSockets,
Winsock), never on the UI or game thread.

## Build

```
cmake -S plugin -B plugin/build -G "Visual Studio 18 2026" -A x64
cmake --build plugin/build --config Release
plugin/build/Release/d2r_rooms_tests.exe
plugin/build/Release/relay_e2e.exe ws://127.0.0.1:8080   # with a relay running (relay/README.md)
```

## Version 0.1 is a probe

What D2R's front-end allows is learned from the first runs, so 0.1 logs every
UI message to `d2rloader/logs/d2r-rooms.log` (the target:command:text of each
button press) and has console commands for trying things:

| Command | What |
|---|---|
| `rooms` | show or hide the rooms list |
| `rooms-ui <panel> [<child>...]` | log a widget's rectangle and input text, e.g. `rooms-ui JoinGameModal JoinGameInputScreen InputText` |
| `rooms-msg <target> <command> [<text>]` | send a UI message as a button would, e.g. `rooms-msg FrontEndNavigation ToTCPIP` |
| `rooms-type <text>` | clear the focused text box and type text |

Open questions it answers: which message opens the TCP/IP screen; whether a
plugin panel opens in the front-end; whether a re-registered layout shows the
new list on reopen; whether typed keys reach the Join Game box.
