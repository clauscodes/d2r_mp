# D2R Rooms

A board of open Diablo II: Resurrected TCP/IP games: who hosts, who plays
(class, level, act, which mod), and the host's address to join. Works with the
D2R VR mods (Authentic, Unleashed) and without them. Players connect their game
directly to the host; no game traffic goes through the board.

Runs free on Cloudflare Workers + D1. Design: [SPEC.md](SPEC.md).

## What is here

| Path | What |
|---|---|
| `worker/src/` | the Worker: API (`board.ts`), input checks (`validate.ts`), the web page (`page.ts`) |
| `worker/migrations/` | the D1 tables |
| `worker/test/` | tests, run inside the Workers runtime with a local D1 |
| `tools/helper.py` | command-line client: `list`, `host`, `join` |
| `.github/workflows/worker.yml` | tests on every push; deploy from `main` once Cloudflare is set up |

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
   Databases → D1); its id goes into `worker/wrangler.jsonc` (`database_id`).
2. An API token (My Profile → API Tokens → *Edit Cloudflare Workers* template,
   plus *Account → D1 → Edit*) and the account id.
3. Repository secrets (Settings → Secrets and variables → Actions):
   `CLOUDFLARE_API_TOKEN`, `CLOUDFLARE_ACCOUNT_ID`, and `ADDR_SALT` (any long
   random string).
4. Push to `main` or press Actions → worker → Run workflow: it tests, applies
   the migrations and deploys to `https://d2r-rooms.<your-subdomain>.workers.dev`.
