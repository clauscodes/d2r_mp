# D2R Rooms relay

Carries a D2R TCP/IP game between a host and its joiners when the host's port cannot be reached from outside. Both PCs
dial out to it, so nobody touches a router. Guide for players and relay owners: [docs/host_a_relay.md](../docs/host_a_relay.md).

| Path | What |
|---|---|
| `PROTOCOL.md` | the wire protocol v1 that every relay speaks |
| `cloudflare/` | the relay as its own Cloudflare Worker: a Durable Object per room, a budget object for the cap |
| `python/` | the same relay in one Python process, with a Dockerfile and Caddy for TLS |
| `conformance.py` | checks any running relay against the protocol (fake host + fake joiners) |

## Develop

```
python -m unittest relay/python/test_relay.py          # the Python relay + conformance, in process

cd relay/cloudflare
npm ci
npx tsc --noEmit
npx vitest run                                         # cost model + sockets inside the Workers runtime
npx wrangler dev                                       # then, in another terminal:
python ../conformance.py ws://127.0.0.1:8787
```

Under `wrangler dev`, two conformance checks (`pipes_and_close` and `keys`) time out. They wait for a close that the
relay starts itself, and the local dev proxy does not pass that close to the Python client. The vitest tests check those
closes inside the runtime, and the plugin's `relay_e2e.exe` sees them through `wrangler dev`. Against a deployed relay,
all 8 checks must pass.

The plugin's own client, end to end with a fake game:

```
plugin/build/Release/relay_e2e.exe ws://127.0.0.1:8080   # python relay/python/relay_server.py (RELAY_PORT=8080)
plugin/build/Release/relay_e2e.exe ws://127.0.0.1:8787   # wrangler dev
```

And a measurement: two joiners on 127.0.0.1 send game-sized packets at a steady rate, the host's fake game echoes
them, and it prints the round trips, the bytes/s and messages/s, and one 768 KB burst:

```
plugin/build/Release/relay_e2e.exe ws://127.0.0.1:8787 --bench 30 25 120   # seconds, packets/s, bytes
```

On one PC (2026-10-30, 2 joiners × 25 packets/s × 120 B): Python relay round trip 0.4 ms median, 0.5 ms at the 95th
percentile; `wrangler dev` 3.8 ms median, 5.9 ms at the 95th percentile, 13 ms worst; nothing lost; a burst of
6.4 MB/s each way through `wrangler dev`. Each joiner was 3.0 KB/s and 25 messages/s each way, and the host 50
messages/s up. What a real D2R game sends is still to be measured: the plugin logs it (plugin/README.md).

## Deploy

Never on a push: a deploy restarts the room objects and drops relayed games. By hand, `npx wrangler deploy` in
`relay/cloudflare`, or Actions → relay → Run workflow with "deploy" ticked. That needs the repository secrets
`CLOUDFLARE_API_TOKEN` (with Workers and Durable Objects edit rights) and `CLOUDFLARE_ACCOUNT_ID`.
