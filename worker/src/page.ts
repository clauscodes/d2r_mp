// The board's web page: the room list, Join (shows the host's address) and
// Create room (keeps the room alive while the page stays open).

export const PAGE = /* html */ `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>D2R Rooms</title>
<style>
:root {
  --bg: #f5f1e8; --panel: #fffdf8; --text: #231d14; --muted: #6d6252; --line: #e2d8c4;
  --accent: #8a5a12; --accent-text: #fff; --warn: #9a3412; --chip: #efe6d3;
  color-scheme: light;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --bg: #14110d; --panel: #1e1a14; --text: #eee5d3; --muted: #a59a85; --line: #3a3226;
    --accent: #d9a441; --accent-text: #1a140b; --warn: #f2994a; --chip: #2b251c;
    color-scheme: dark;
  }
}
:root[data-theme="dark"] {
  --bg: #14110d; --panel: #1e1a14; --text: #eee5d3; --muted: #a59a85; --line: #3a3226;
  --accent: #d9a441; --accent-text: #1a140b; --warn: #f2994a; --chip: #2b251c;
  color-scheme: dark;
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text); font: 15px/1.45 system-ui, sans-serif; }
main { max-width: 860px; margin: 0 auto; padding: 20px 16px 48px; }
h1 { font-size: 24px; margin: 0 0 4px; letter-spacing: .5px; }
.sub { color: var(--muted); margin: 0 0 18px; }
.bar { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; margin-bottom: 14px; }
select, input, button { font: inherit; color: inherit; }
select, input { background: var(--panel); border: 1px solid var(--line); border-radius: 8px; padding: 7px 9px; min-width: 0; }
button { background: var(--chip); border: 1px solid var(--line); border-radius: 8px; padding: 7px 12px; cursor: pointer; }
button.primary { background: var(--accent); color: var(--accent-text); border-color: var(--accent); font-weight: 600; }
button:disabled { opacity: .5; cursor: default; }
.grow { flex: 1; }
.card { background: var(--panel); border: 1px solid var(--line); border-radius: 12px; padding: 14px 16px; margin-bottom: 12px; }
.head { display: flex; flex-wrap: wrap; gap: 6px 12px; align-items: baseline; }
.name { font-weight: 650; font-size: 17px; }
.code { font-family: ui-monospace, monospace; letter-spacing: 2px; color: var(--accent); }
.meta { color: var(--muted); font-size: 13px; }
.chip { display: inline-block; background: var(--chip); border-radius: 999px; padding: 1px 8px; font-size: 12px; margin-left: 4px; }
table { width: 100%; border-collapse: collapse; margin-top: 8px; font-size: 14px; }
td { padding: 4px 6px 4px 0; border-top: 1px solid var(--line); vertical-align: top; }
td.r { text-align: right; white-space: nowrap; }
.join { margin-top: 10px; display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
.addr { font-family: ui-monospace, monospace; font-size: 16px; background: var(--chip); padding: 6px 10px; border-radius: 8px; user-select: all; }
.warn { color: var(--warn); font-size: 13px; }
.empty { color: var(--muted); text-align: center; padding: 28px 0; }
details.card summary { cursor: pointer; font-weight: 600; }
form { display: grid; grid-template-columns: repeat(auto-fit, minmax(170px, 1fr)); gap: 10px; margin-top: 12px; }
label { display: grid; gap: 3px; font-size: 13px; color: var(--muted); }
label.check { display: flex; gap: 6px; align-items: center; }
.full { grid-column: 1 / -1; }
.hosting { border-color: var(--accent); }
</style>
</head>
<body>
<main>
  <h1>D2R Rooms</h1>
  <p class="sub">Open Diablo II: Resurrected TCP/IP games. Join shows the host's address &mdash; in D2R choose TCP/IP, Join Game, and enter it.</p>

  <div id="hosting"></div>

  <details class="card" id="create-box">
    <summary>Create a room</summary>
    <form id="create">
      <label class="full">Room name<input name="name" maxlength="32" required placeholder="Baal runs"></label>
      <label>Mod<select name="mod">
        <option value="d2r-vr">D2R VR Authentic</option>
        <option value="d2r-pcvr">D2R VR Unleashed</option>
        <option value="none">No VR mod</option>
      </select></label>
      <label>Mod version<input name="version" maxlength="24" placeholder="0.175.0"></label>
      <label>Difficulty<select name="difficulty"><option>normal</option><option>nightmare</option><option>hell</option></select></label>
      <label>Mode<select name="mode"><option>softcore</option><option>hardcore</option></select></label>
      <label>Act<select name="act"><option value="">&ndash;</option><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option></select></label>
      <label>Max players<select name="max_players"><option>8</option><option>7</option><option>6</option><option>5</option><option>4</option><option>3</option><option>2</option></select></label>
      <label>Your character<input name="char" maxlength="32" placeholder="optional"></label>
      <label>Class<select name="cls"><option value="unknown">&ndash;</option><option>amazon</option><option>assassin</option><option>barbarian</option><option>druid</option><option>necromancer</option><option>paladin</option><option>sorceress</option></select></label>
      <label>Level<input name="level" type="number" min="1" max="99" placeholder="optional"></label>
      <label>Password<input name="password" maxlength="64" placeholder="optional"></label>
      <label class="full">Address players connect to<input name="address" maxlength="64" placeholder="leave empty: your internet address is used (or your VPN address, e.g. Radmin / ZeroTier)"></label>
      <label class="check"><input type="checkbox" name="public" checked> Show in the public list</label>
      <div class="full"><button class="primary" type="submit">Create room</button> <span class="meta">The room stays listed while this page is open.</span></div>
    </form>
  </details>

  <div class="bar">
    <select id="f-mod" aria-label="Mod">
      <option value="">All mods</option><option value="d2r-vr">Authentic</option><option value="d2r-pcvr">Unleashed</option><option value="none">No VR mod</option>
    </select>
    <select id="f-diff" aria-label="Difficulty">
      <option value="">All difficulties</option><option>normal</option><option>nightmare</option><option>hell</option>
    </select>
    <span class="grow"></span>
    <button id="refresh" type="button">Refresh</button>
  </div>
  <div id="rooms"><div class="empty">Loading&hellip;</div></div>
</main>
<script>
const MODS = { "d2r-vr": "Authentic", "d2r-pcvr": "Unleashed", "none": "no VR mod" };
const $ = (s) => document.querySelector(s);
const el = (tag, props = {}, ...kids) => {
  const e = Object.assign(document.createElement(tag), props);
  for (const k of kids) if (k != null) e.append(k);
  return e;
};
const joined = new Map(); // room_id -> the join answer, while the page is open
const modLabel = (m) => m ? (MODS[m.id] || m.id) + (m.version ? " " + m.version : "") : "?";
const cap = (s) => s ? s[0].toUpperCase() + s.slice(1) : "";

async function api(method, path, body, token) {
  const headers = { "content-type": "application/json" };
  if (token) headers.authorization = "Bearer " + token;
  const r = await fetch(path, { method, headers, body: body ? JSON.stringify(body) : undefined });
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw Object.assign(new Error(data.error || r.status), { data, status: r.status });
  return data;
}

function addressText(a) {
  const host = a.address.includes(":") ? "[" + a.address + "]" : a.address;
  return a.port === 4000 ? a.address : host + ":" + a.port;
}

function roomCard(room) {
  const card = el("div", { className: "card" });
  card.append(el("div", { className: "head" },
    el("span", { className: "name", textContent: room.name + (room.locked ? " \\u{1F512}" : "") }),
    el("span", { className: "code", textContent: room.code, title: "Room code" }),
    el("span", { className: "meta", textContent:
      [cap(room.difficulty), cap(room.mode), room.act ? "Act " + room.act : null,
       room.players_count + "/" + room.max_players + " players", modLabel(room.mod)].filter(Boolean).join(" \\u00b7 ") })));
  if (room.players.length) {
    const tb = el("tbody");
    for (const p of room.players) {
      tb.append(el("tr", {},
        el("td", {}, el("b", { textContent: p.name }), p.host ? el("span", { className: "chip", textContent: "host" }) : null),
        el("td", { textContent: [cap(p.class === "unknown" ? "" : p.class), p.level].filter(Boolean).join(" ") }),
        el("td", { textContent: [p.act ? "Act " + p.act : null, p.area || null].filter(Boolean).join(", ") }),
        el("td", { className: "r" },
          el("span", { className: "chip", textContent: modLabel(p.mod) }),
          p.vr === true ? el("span", { className: "chip", textContent: "VR" }) : null)));
    }
    card.append(el("table", {}, tb));
  }
  const out = el("div", { className: "join" });
  const showAddress = (a) => {
    const copy = el("button", { type: "button", textContent: "Copy" });
    copy.onclick = () => navigator.clipboard?.writeText(a.address).then(() => copy.textContent = "Copied");
    out.replaceChildren(el("span", { className: "addr", textContent: addressText(a) }), copy,
      el("span", { className: "meta", textContent: "In D2R: Play \\u2192 TCP/IP \\u2192 Join Game \\u2192 this address" }));
    if (a.address_ipv6) out.append(el("div", { className: "warn", textContent: "This is an IPv6 address; D2R may need the host's IPv4 or VPN address instead." }));
  };
  if (joined.has(room.room_id)) {
    showAddress(joined.get(room.room_id));
    card.append(out);
    return card;
  }
  const btn = el("button", { className: "primary", type: "button", textContent: "Join" });
  btn.onclick = async () => {
    let password = "";
    if (room.locked) {
      password = prompt("Room password") ?? "";
      if (!password) return;
    }
    btn.disabled = true;
    try {
      const a = await api("POST", "/api/join", { room_id: room.room_id, password });
      joined.set(room.room_id, a); // kept across list refreshes
      showAddress(a);
    } catch (e) {
      out.append(el("span", { className: "warn", textContent: e.status === 403 ? "Wrong password." : e.status === 429 ? "Too many attempts, wait a minute." : "Room is gone." }));
      btn.disabled = false;
    }
  };
  out.append(btn);
  card.append(out);
  return card;
}

async function load() {
  const q = new URLSearchParams();
  if ($("#f-mod").value) q.set("mod", $("#f-mod").value);
  if ($("#f-diff").value) q.set("difficulty", $("#f-diff").value);
  try {
    const { rooms } = await api("GET", "/api/rooms?" + q);
    $("#rooms").replaceChildren(...(rooms.length ? rooms.map(roomCard) : [el("div", { className: "empty", textContent: "No open rooms right now." })]));
  } catch {
    $("#rooms").replaceChildren(el("div", { className: "empty", textContent: "Could not load the rooms." }));
  }
}

let hosting = null;

function showHosting() {
  const box = $("#hosting");
  if (!hosting) { box.replaceChildren(); $("#create-box").hidden = false; return; }
  $("#create-box").hidden = true;
  const close = el("button", { type: "button", textContent: "Close room" });
  close.onclick = async () => {
    clearInterval(hosting.timer);
    await api("DELETE", "/api/rooms/" + hosting.room_id, null, hosting.host_token).catch(() => {});
    hosting = null;
    showHosting();
    load();
  };
  const card = el("div", { className: "card hosting" },
    el("div", { className: "head" },
      el("span", { className: "name", textContent: "You are hosting" }),
      el("span", { className: "code", textContent: hosting.code })),
    el("p", { className: "meta", textContent: "Players connect to " + addressText(hosting) +
      ". Host a TCP/IP game in D2R and keep this page open; closing it removes the room within a few minutes." }),
    close);
  if (hosting.address_ipv6) card.append(el("div", { className: "warn", textContent: "Your address was seen as IPv6. If players cannot connect, enter your IPv4 or VPN address when creating the room." }));
  box.replaceChildren(card);
}

$("#create").onsubmit = async (ev) => {
  ev.preventDefault();
  const f = new FormData(ev.target);
  const body = {
    name: f.get("name"), mod: { id: f.get("mod"), version: f.get("version") || "" },
    difficulty: f.get("difficulty"), mode: f.get("mode"), max_players: Number(f.get("max_players")),
    public: f.get("public") === "on", password: f.get("password") || "",
  };
  if (f.get("act")) body.act = Number(f.get("act"));
  if (f.get("address")) body.address = f.get("address");
  if (f.get("char")) body.player = { name: f.get("char"), class: f.get("cls"), level: f.get("level") ? Number(f.get("level")) : null };
  try {
    hosting = await api("POST", "/api/rooms", body);
    hosting.timer = setInterval(() => api("POST", "/api/rooms/" + hosting.room_id + "/heartbeat", {}, hosting.host_token)
      .catch((e) => { if (e.status === 404) { clearInterval(hosting.timer); hosting = null; showHosting(); } }),
      (hosting.heartbeat_every || 60) * 1000);
    showHosting();
    load();
  } catch (e) {
    const msg = { too_many_rooms: "You already host 3 rooms.", too_many_creates: "Too many rooms created, wait a minute.", invalid: "Check the fields: " + (e.data?.detail?.[0]?.loc || "") }[e.message] || "Could not create the room.";
    alert(msg);
  }
};

$("#f-mod").onchange = load;
$("#f-diff").onchange = load;
$("#refresh").onclick = load;
load();
setInterval(load, 20000);
</script>
</body>
</html>
`;
