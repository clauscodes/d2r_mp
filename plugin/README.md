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
SharedEvent, Widget, Thread, Http, Lifecycle).

The board address defaults to `https://d2r-rooms.d2r-rooms.workers.dev`; the
environment variable `D2R_ROOMS_SERVER` points it elsewhere.

## Build

```
cmake -S plugin -B plugin/build -G "Visual Studio 18 2026" -A x64
cmake --build plugin/build --config Release
plugin/build/Release/d2r_rooms_tests.exe
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
