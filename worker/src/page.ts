// The board's web page: only the list - each room's mod version, players,
// characters and levels. Rooms are created and joined from the game by the
// D2R Rooms plugin (a room may go through a relay, which only the plugin can use).

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
.empty { color: var(--muted); text-align: center; padding: 28px 0; }
</style>
</head>
<body>
<main>
  <h1>D2R Rooms</h1>
  <p class="sub">Open Diablo II: Resurrected games. Create and join them in the game: Multiplayer &rarr; D2R Rooms (the D2R Rooms plugin).</p>

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

function roomCard(room) {
  const card = el("div", { className: "card" });
  card.append(el("div", { className: "head" },
    el("span", { className: "name", textContent: room.name + (room.locked ? " \\u{1F512}" : "") }),
    el("span", { className: "code", textContent: room.code, title: "Room code" }),
    room.relay_url ? el("span", { className: "chip", textContent: "relay" }) : null,
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

$("#f-mod").onchange = load;
$("#f-diff").onchange = load;
$("#refresh").onclick = load;
load();
setInterval(load, 20000);
</script>
</body>
</html>
`;
