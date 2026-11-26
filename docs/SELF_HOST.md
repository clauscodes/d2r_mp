# Run your own D2R Rooms server

The public room list runs on Cloudflare's free plan, and that plan has a daily limit. When many people play, the list
can run out for the rest of the day. A group of friends can run its own list in about 15 minutes. It costs nothing, and
the group then has its own free daily limit.

You need a free Cloudflare account and a Windows PC. You don't need a server of your own or a credit card.

- [1. What you get](#1-what-you-get)
- [2. Make a free Cloudflare account](#2-make-a-free-cloudflare-account)
- [3. Put the server on Cloudflare](#3-put-the-server-on-cloudflare)
- [4. Point your group's games at it](#4-point-your-groups-games-at-it)
- [5. What the free plan allows](#5-what-the-free-plan-allows)
- [6. Update it later](#6-update-it-later)
- [Another way: deploy from GitHub](#another-way-deploy-from-github)

## 1. What you get

- Your own room list at `https://d2r-rooms.<your-name>.workers.dev`. It works the same as the public one: **Create** and
  **Join** in the game, and a web page that shows the open rooms.
- Players who set your address see only your rooms. The public list doesn't show them, and your players don't see the
  public rooms.
- Games still run between the players' PCs, as before. The list only tells the game where to connect.

## 2. Make a free Cloudflare account

1. Sign up at <https://dash.cloudflare.com/sign-up> and confirm your email.
2. In the dashboard, open **Workers & Pages** once. Cloudflare asks you to pick a **workers.dev subdomain**, for example
   `ourclan`. Your server's address will be `https://d2r-rooms.ourclan.workers.dev`.

The free plan is enough. Don't add a payment method.

## 3. Put the server on Cloudflare

1. Install **Node.js** (the LTS version) from <https://nodejs.org>. Keep the default options.
2. Download this repository: on <https://github.com/clauscodes/d2r_mp> press **Code → Download ZIP**, and unpack it,
   for example to `C:\d2r_mp`.
3. Open **PowerShell** in the `worker` folder (`C:\d2r_mp\worker`; in Explorer, Shift + right-click the folder →
   *Open in Terminal*), and install the tools:

   ```powershell
   npm ci
   ```

4. Sign in to Cloudflare. A browser page opens; press **Allow**:

   ```powershell
   npx wrangler login
   ```

5. Create the database the list keeps its rooms in:

   ```powershell
   npx wrangler d1 create d2r-rooms
   ```

   It prints a `database_id` (a long id like `1677...-...`). Copy it.

6. Open `worker\wrangler.jsonc` in Notepad and change two lines:
   - delete the whole `"account_id": "...",` line. Wrangler then uses the account you signed in with;
   - replace the `database_id` value with the id you copied.

   Save the file.

7. Deploy:

   ```powershell
   npm run deploy
   ```

   When it asks whether to apply the migrations (create the tables), answer `y`. At the end it prints your address,
   `https://d2r-rooms.<your-name>.workers.dev`. Open it in a browser. "No open rooms right now." means the server
   works.

8. Optional, but a good idea: set a private salt for the hashed player addresses the server uses to limit spam. Run
   this, then type any long random text and press Enter:

   ```powershell
   npx wrangler secret put ADDR_SALT
   ```

## 4. Point your group's games at it

Every player in the group does this once. You need D2R Rooms plugin 0.9.12 or newer.

1. Open `<game folder>\d2rloader\plugins\d2r-rooms.ini` in Notepad. The plugin writes this file the first time the
   game starts.
2. Find the `[board]` section, or add it at the end, and put your address after `url =`:

   ```ini
   [board]
   url = https://d2r-rooms.ourclan.workers.dev
   ```

3. Save, and restart the game.

The D2R Rooms window now lists your group's rooms. To go back to the public list, leave `url =` empty and restart.

A host and the players who join must use the same list: a room is visible only on the list it was created on.

Relays (for hosts whose PC can't be reached from the internet) are a separate setting. Your list hands out whatever relay
the host's own `d2r-rooms.ini` uses. To run your own relay too, see [host_a_relay.md](host_a_relay.md).

## 5. What the free plan allows

Cloudflare's free plan, per account, per day (it resets at 00:00 UTC):

| | Free plan |
|---|---|
| Requests to the Worker | 100,000 a day |
| Database rows written | 100,000 a day |
| Database rows read | 5 million a day |
| Database size | 5 GB |

Current numbers: [Workers limits](https://developers.cloudflare.com/workers/platform/limits/) and
[D1 limits](https://developers.cloudflare.com/d1/platform/limits/).

What D2R Rooms uses:

- A hosted room sends one heartbeat a minute, which is one request and one row written. One room open for an hour costs
  about 60 of each. The writes limit covers about 1,600 room-hours a day.
- Every Create, Join and list refresh in the game is one request.
- A browser tab with the web page open asks for the list every 20 seconds, about 180 requests an hour.

A group of friends stays far below these limits. When an account reaches a limit, its list stops answering until the
next reset, and games that are already running go on. Cloudflare's paid Workers plan ($5 a month) raises the limits if
you ever need that.

The server's own limits are 500 rooms, 3 rooms per player address and 5 wrong passwords per minute. You can change them
under `"vars"` in `wrangler.jsonc` (`MAX_ROOMS`, `MAX_ROOMS_PER_ADDR`, `JOIN_FAIL_LIMIT`), then run `npm run deploy` again.

## 6. Update it later

When this repository gets a new version, download it again, put your `database_id` into the new `wrangler.jsonc` and
delete its `account_id` line as in step 3.6, then run `npm ci` and `npm run deploy` in the `worker` folder. The rooms in
your database stay.

## Another way: deploy from GitHub

If you prefer not to install anything, GitHub can deploy for you:

1. Fork this repository on GitHub, and on your fork's **Actions** tab, allow workflows to run.
2. In the Cloudflare dashboard: **Storage & Databases → D1 → Create**, name `d2r-rooms`, and copy its id. Your account
   id is on the right side of the **Workers & Pages** page.
3. In your fork, edit `worker/wrangler.jsonc`: put your `account_id` and the new `database_id`.
4. Create an API token: **My Profile → API Tokens → Create Token**, use the *Edit Cloudflare Workers* template, and add
   the permission *Account → D1 → Edit*.
5. In your fork: **Settings → Secrets and variables → Actions → New repository secret**. Add `CLOUDFLARE_API_TOKEN` (the
   token) and `ADDR_SALT` (any long random text).
6. Go to **Actions → worker → Run workflow**. It tests, creates the tables and deploys. Your address is
   `https://d2r-rooms.<your-name>.workers.dev`.

Then continue with [step 4](#4-point-your-groups-games-at-it).
