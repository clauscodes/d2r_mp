# D2R Rooms

A board of open Diablo II: Resurrected TCP/IP games: who hosts, who plays
(class, level, act, which mod), and the host's address to join. Works with the
D2R VR mods (Authentic, Unleashed) and without them. Players connect their game
directly to the host; no game traffic goes through the board. When the host's
port cannot be reached from outside, the game goes through a relay instead:
both PCs dial out, nobody touches a router ([relay/](relay/README.md), guide for
running your own: [docs/host_a_relay.md](docs/host_a_relay.md)).

Runs free on Cloudflare Workers + D1. Design: [SPEC.md](SPEC.md).

## Install the plugin (players)

**Download: [d2rl-d2r-rooms.dll](https://github.com/clauscodes/d2r_mp/raw/main/download/d2rl-d2r-rooms.dll)**
(plugin 0.9.8, Windows x64, [SHA-256](https://github.com/clauscodes/d2r_mp/raw/main/download/d2rl-d2r-rooms.dll.sha256);
sample settings: [d2r-rooms.ini](https://github.com/clauscodes/d2r_mp/raw/main/download/d2r-rooms.ini)).
Open rooms: <https://d2r-rooms.d2r-rooms.workers.dev/>.

1. Needs [D2RLoader](https://d2rloader.net/) 1.3.1 or newer. Close the game.
2. Copy `d2rl-d2r-rooms.dll` into `<game>\d2rloader\plugins\`, beside any VR mod's
   plugin already there (D2R VR Authentic, D2R VR Unleashed, or none).
3. Start the game. The plugin writes `d2r-rooms.ini` beside itself on its first start;
   the sample above is the same file.

- **Host:** D2RLoader's **Multiplayer** → *HOST GAME* → **CREATE GAME**. The room is
  listed on the board while you play. When friends cannot reach your PC (no port
  forwarding, a router without UPnP), the game goes through a relay: both PCs dial
  out, nobody touches a router.
- **Join:** D2RLoader's **Multiplayer** → the **D2R Rooms** tab (or **F9**) → **Join** on a room.
  For a locked room, type its password first. The plugin opens *Direct Join* and
  types the address (`127.0.0.1` when the room uses a relay).
- **`mode`** in `d2r-rooms.ini`, `[relay]` section: `auto` (the default) uses a relay
  only when D2RLoader reports that your port cannot be reached from outside;
  `always` hosts every room through a relay; `never` never does. `enabled = no`
  turns the plugin's part of *CREATE GAME* off.
- **Everyone in one game needs the same game version and the same VR mod version**
  (or no mod for all); otherwise D2R says *Game Data Does Not Match*. The rooms list
  shows each room's VR version.
- Both the host and every joiner of a relayed room need the plugin.

More: [plugin/README.md](plugin/README.md); your own relay: [docs/host_a_relay.md](docs/host_a_relay.md).

## What is here

| Path | What |
|---|---|
| `worker/src/` | the Worker: API (`board.ts`), input checks (`validate.ts`), the web page (`page.ts`) |
| `worker/migrations/` | the D1 tables |
| `worker/test/` | tests, run inside the Workers runtime with a local D1 |
| `plugin/` | the D2RLoader plugin: the list beside D2R's TCP/IP screen, the relay client |
| `relay/` | the relay: protocol, Cloudflare Worker, Python/Docker server, conformance check |
| `tools/helper.py` | command-line client: `list`, `host`, `join` |
| `.github/workflows/worker.yml` | tests on every push; deploy from `main` once Cloudflare is set up |
| `.github/workflows/relay.yml` | relay tests on every push; deploy only by hand (Run workflow) |

## Develop

```
cd worker
npm ci
npx vitest run
npx wrangler d1 migrations apply DB --local
npx wrangler dev
```

The page is then at http://127.0.0.1:8787/.

## Helper

```
pip install -r tools/requirements.txt
python tools/helper.py list --server https://<your-worker>.workers.dev
python tools/helper.py host --server URL --name "Baal runs" --mod d2r-vr --version 0.175.0
python tools/helper.py join --server URL --code K7QX --mod d2r-pcvr
```

`host` keeps the room alive until Ctrl+C. Players behind the host's router need
the D2R port open on the host, or everyone in one VPN (`--address` gives the
host's VPN address).

## Deploy (once)

1. A free Cloudflare account and a D1 database `d2r-rooms` (Storage &
   Databases → D1); the account id and the database id go into
   `worker/wrangler.jsonc` (`account_id`, `database_id`).
2. An API token (My Profile → API Tokens → *Edit Cloudflare Workers* template,
   plus *Account → D1 → Edit*).
3. Repository secrets (Settings → Secrets and variables → Actions):
   `CLOUDFLARE_API_TOKEN` and `ADDR_SALT` (any long random string).
4. Push to `main` or press Actions → worker → Run workflow: it tests, applies
   the migrations and deploys to `https://d2r-rooms.<your-subdomain>.workers.dev`.
