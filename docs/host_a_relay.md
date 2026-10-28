# Host a D2R Rooms relay

A **relay** lets two players share a D2R TCP/IP game when the host's PC cannot be reached from the internet. That happens
when the router has no port forwarding, UPnP is off, or the internet provider uses carrier-grade NAT. Both PCs dial out to
the relay, and the relay passes the game's bytes between them. Nothing in the game changes.

D2R Rooms has one relay of its own. Anyone can run another, for friends or for everyone, and players add it in one line.
This page shows how.

- [1. When a relay is used](#1-when-a-relay-is-used)
- [2. Add a relay to your game](#2-add-a-relay-to-your-game)
- [3. Run a relay on Cloudflare](#3-run-a-relay-on-cloudflare)
- [4. Run a relay on your own server (Docker)](#4-run-a-relay-on-your-own-server-docker)
- [5. Check that it works](#5-check-that-it-works)
- [6. What the status means](#6-what-the-status-means)
- [7. Costs and limits](#7-costs-and-limits)

## 1. When a relay is used

1. The host presses **Create** in the D2R Rooms list and starts the game as usual.
2. D2RLoader opens the router by itself (UPnP, PCP or NAT-PMP), then checks the port from outside.
3. If that check says **reachable**, friends connect straight to the host and no relay is used.
4. If it says **unreachable** (for example "no compatible router was found"), the D2R Rooms plugin opens the room on the
   first relay in the host's list that answers and takes new rooms. The room in the list then shows `relay: active`.
5. A friend presses **Join**. The plugin connects to the host's relay, and the game joins `127.0.0.1`. The plugin types
   that into the Join Game box by itself.

Every player who uses a relay needs the D2R Rooms plugin (`d2rl-d2r-rooms.dll` in `<game>\d2rloader\plugins\`). This
works with any mod (D2R VR Authentic, D2R VR Unleashed, or none). There is nothing else to install: no VPN, no router
settings and no admin rights.

## 2. Add a relay to your game

The plugin writes `d2r-rooms.ini` beside its DLL the first time the game starts:
`<game>\d2rloader\plugins\d2r-rooms.ini`. Open it in Notepad:

```ini
[relay]
; auto   = only when D2RLoader reports that your port cannot be reached from outside
; always = every room you host goes through a relay
; never  = never (friends then need your port open)
mode = auto

[relays]
; One relay per line:  name = wss://address [key]
D2R Rooms = wss://d2r-rooms-relay.d2r-rooms.workers.dev
```

- **Add a relay:** one line under `[relays]`, as `name = wss://address`. A private relay also needs its key after a space:
  `Friends = wss://relay.friends.net their-key-here`.
- **Order matters for hosting.** The first relay that answers and takes new rooms gets your room. Put yours first to
  prefer it, or delete ours if you never want it.
- **Joining always uses the host's relay**, whatever is in your own list. You do not need the host's relay in your list,
  or its key: the room gives you a one-room pass.
- Save the file and restart the game. The D2R Rooms panel shows each relay's status on its status line, for example
  `Relays: Friends active, D2R Rooms near its limit`.

## 3. Run a relay on Cloudflare

This needs no server to look after, and players everywhere get a nearby connection, because the relay room is created
close to the host.

**What you need:** a free Cloudflare account, and [Node.js](https://nodejs.org) 22 or newer on your PC.

1. Get the code: download the D2R Rooms repository (Code → Download ZIP) and unpack it. Open a terminal in
   `relay\cloudflare`.
2. Install the tools:
   ```
   npm ci
   ```
3. Log in to Cloudflare (a browser opens):
   ```
   npx wrangler login
   ```
4. Choose the plan in `wrangler.toml`:
   - `PLAN = "free"`: $0. The Free plan allows 100 000 requests a day, about **11 hours of one joiner's play a day** in
     total. The relay stops taking new rooms and joiners near 90% of that quota. **Past the quota, Cloudflare cuts games
     in progress**, so Free suits a small group.
   - `PLAN = "paid"` (default): Workers Paid, $5 a month (Cloudflare dashboard → Workers & Pages → Plans). It includes
     about 110 joiner-hours, and each joiner-hour after that costs about $0.007. `BUDGET_USD` is the whole monthly bill
     you allow, the $5 included (default `"10"`).
   - `NEAR_LIMIT = "0.8"`: at 80% of the cap, the relay takes no new rooms. Rooms already made still take joiners.
     Running games are never cut by the relay itself.
5. Optional, to keep the relay for your friends only. Pick a key (16 or more letters and digits), then:
   ```
   npx wrangler secret put RELAY_KEYS
   ```
   Type the key. Several keys, separated by commas, are fine. Only **hosts** need the key; joiners never do.
6. Deploy:
   ```
   npx wrangler deploy
   ```
   It prints the address, such as `https://d2r-rooms-relay.<your-name>.workers.dev`. Players add it with `wss://` in
   front of the name instead of `https://`.
7. Check it (§5), then share the line for `d2r-rooms.ini`, for example
   `My relay = wss://d2r-rooms-relay.<your-name>.workers.dev`.

**Redeploying drops the games in progress** (Cloudflare restarts the rooms). Deploy when nobody plays.

Good to set as well: Cloudflare dashboard → Manage account → Billing → **billing notifications**. The relay's own cap is an
estimate from its counters; Cloudflare's bill is the real one.

## 4. Run a relay on your own server (Docker)

This suits a cheap VPS: a flat price, any number of hours, one region. It speaks exactly the same protocol, so players
cannot tell it from the Cloudflare one.

**What you need:** a Linux server with Docker and a public IPv4 address, and a domain name (or a free subdomain, for
example from DuckDNS) pointing at it. Ports 80 and 443 must be open.

1. Copy the `relay/python` folder to the server.
2. Make the settings file:
   ```
   cp .env.example .env
   nano .env
   ```
   - `RELAY_DOMAIN`: the domain name pointing at this server (needed for the certificate).
   - `RELAY_NAME`: what players see.
   - `RELAY_KEYS`: keys for hosts, separated by commas. Leave it empty for a relay open to every host.
   - `MAX_ROOMS`: rooms at once (default 500). At `NEAR_LIMIT` (0.8) of it, no new rooms are taken.
3. Start it:
   ```
   docker compose up -d
   ```
   Caddy gets a Let's Encrypt certificate by itself within a minute.
4. Check it (§5). Players add `My relay = wss://<RELAY_DOMAIN>`.

Without Docker: `pip install websockets`, then `RELAY_PORT=8080 python relay_server.py`, behind any reverse proxy that
passes WebSockets and adds TLS. Plain `ws://` works only on a LAN.

## 5. Check that it works

From any PC with Python and `pip install websockets`:

```
python relay/conformance.py wss://your-relay-address
python relay/conformance.py wss://your-relay-address --relay-key YOUR-KEY
```

It plays a fake host and fake joiners against the relay and must end with `8/8 checks passed`. Then
`https://your-relay-address/healthz` in a browser shows the status, for example
`{"ok":true,"relay":1,"status":"active",...}`.

With a Windows build of the plugin, `relay_e2e.exe wss://your-relay-address` also sends real TCP bytes through the
plugin's own relay client, with a fake game, and must end with `all checks passed`.

## 6. What the status means

The D2R Rooms panel shows it per relay, and the room list shows it for every room that uses a relay.

| Status | New rooms | New joiners | Games already running |
|---|---|---|---|
| **active** | yes | yes | go on |
| **near its limit** | no: hosts use the next relay in their list | yes | go on |
| **limit reached** | no | no: joiners try the host's own address | go on |
| **not answering** | no | no | the relay is down or unreachable |

## 7. Costs and limits

| | Cloudflare Free | Cloudflare Paid | Your VPS |
|---|---|---|---|
| Monthly price | $0 | $5 minimum, then use; default cap $10 | the server, about €4-5 (2 vCPU, 20 TB traffic, EU) |
| How much play | ~11 joiner-hours a day | ~110 joiner-hours included; ~$0.007 per joiner-hour after | as much as the server's bandwidth allows (a few hundred joiners) |
| Traffic | free | free | inside the server's allowance |
| Where | near each host, worldwide | near each host, worldwide | one place: far players get more delay |
| Looking after | none | none | updates, monitoring |

Per player, the game sends a few KB/s each way (estimated 5-20 KB/s; not measured yet). A relay adds roughly 10-40 ms
round trip in one region. Each relay allows 7 joiners per room (D2R's 8 players) and 256 KiB/s per joiner, far above what
the game uses.

**Privacy.** A relay sees only the encrypted connection to itself and the game's bytes. It keeps no list of rooms or
players, and logs nothing by default. Joiners never get the host's IP address from a relay room.

The protocol both relays speak is [relay/PROTOCOL.md](../relay/PROTOCOL.md). A relay that passes `conformance.py` works
with the plugin.
