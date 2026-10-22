import { env } from "cloudflare:test";
import { exports } from "cloudflare:workers";
import { beforeEach, describe, expect, it } from "vitest";

const ROOM = { name: "Baal runs", mod: { id: "d2r-vr", version: "0.175.0" }, difficulty: "hell" };
const JOIN_MOD = { id: "d2r-pcvr", version: "0.3.19" };
const HOST_IP = "203.0.113.7";

async function call(method: string, path: string, body?: unknown, opts: { ip?: string; token?: string; raw?: string } = {}) {
  const headers: Record<string, string> = { "content-type": "application/json", "cf-connecting-ip": opts.ip ?? HOST_IP };
  if (opts.token) headers.authorization = "Bearer " + opts.token;
  const res = await exports.default.fetch(
    new Request("https://rooms.test" + path, { method, headers, body: opts.raw ?? (body === undefined ? undefined : JSON.stringify(body)) }),
  );
  const text = await res.text();
  let json: any = null;
  try {
    json = JSON.parse(text);
  } catch {}
  return { status: res.status, json, text };
}

async function create(over: Record<string, unknown> = {}, ip = HOST_IP) {
  const r = await call("POST", "/api/rooms", { ...ROOM, ...over }, { ip });
  expect(r.status, r.text).toBe(200);
  return r.json;
}

const join = (body: Record<string, unknown>, ip = "198.51.100.20") => call("POST", "/api/join", { mod: JOIN_MOD, ...body }, { ip });
const list = async (query = "") => (await call("GET", "/api/rooms" + query)).json.rooms as any[];
const byCode = (code: string) => call("GET", "/api/rooms/by-code/" + code);
const heartbeat = (room: any, body: unknown = {}, token = room.host_token) =>
  call("POST", `/api/rooms/${room.room_id}/heartbeat`, body, { token });
const age = (room_id: string, seconds: number) =>
  env.DB.prepare("UPDATE rooms SET last_seen = last_seen - ? WHERE room_id = ?").bind(seconds, room_id).run();

beforeEach(async () => {
  await env.DB.batch([env.DB.prepare("DELETE FROM rooms"), env.DB.prepare("DELETE FROM attempts")]);
});

describe("create and find", () => {
  it("gives a code, a token and the host's address", async () => {
    const room = await create();
    expect(room.code).toMatch(/^[ABCDEFGHJKLMNPQRSTUVWXYZ23456789]{4}$/);
    expect(room.host_token.length).toBeGreaterThanOrEqual(30);
    expect(room).toMatchObject({ address: HOST_IP, port: 4000, address_ipv6: false, heartbeat_every: 60 });
    const r = await byCode(room.code.toLowerCase()); // typed by hand: any case
    expect(r.status).toBe(200);
    expect(r.json.room).toMatchObject({ name: "Baal runs", difficulty: "hell", mode: "softcore", locked: false });
    expect(r.json.room.mod).toEqual(ROOM.mod);
    expect(r.text).not.toContain(HOST_IP);
    expect(r.text).not.toContain(room.host_token);
  });

  it("stores no token in clear", async () => {
    const room = await create();
    const row = await env.DB.prepare("SELECT * FROM rooms").first<any>();
    expect(JSON.stringify(row)).not.toContain(room.host_token);
    expect(JSON.stringify(row)).not.toContain(HOST_IP.replace(/\./g, "")); // addr_hash is a hash
    expect(row.address).toBe(HOST_IP);
  });

  it("takes an address the host gives and flags IPv6", async () => {
    expect(await create({ address: "26.12.0.5", port: 4001 })).toMatchObject({ address: "26.12.0.5", port: 4001 });
    expect(await create({}, "2001:db8::7")).toMatchObject({ address: "2001:db8::7", address_ipv6: true });
    expect((await create({ address: "my-pc.example.net" }, "198.51.100.1")).address).toBe("my-pc.example.net");
  });

  it("cleans names", async () => {
    const room = await create({ name: "  Baal\u0000‮   runs \n" });
    expect((await byCode(room.code)).json.room.name).toBe("Baal runs");
  });

  it("refuses bad input without echoing it", async () => {
    let r = await call("POST", "/api/rooms", { ...ROOM, name: "\u0001\u0002", password: "secret-pw" });
    expect(r.status).toBe(422);
    expect(r.json.error).toBe("invalid");
    expect(r.text).not.toContain("secret-pw");
    for (const bad of [{ mod: { id: "Bad Mod!" } }, { max_players: 9 }, { address: "999.1.1.1" }, { address: "1234" },
      { port: 0 }, { difficulty: "inferno" }, { player: { name: "x", class: "Paladin!" } }, { player: { name: "x", level: 100 } }]) {
      r = await call("POST", "/api/rooms", { ...ROOM, ...bad });
      expect(r.status, JSON.stringify(bad)).toBe(422);
    }
    r = await call("POST", "/api/rooms", undefined, { raw: "{not json" });
    expect(r.status).toBe(422);
  });

  it("limits rooms per address", async () => {
    for (let i = 0; i < 3; i++) await create();
    const r = await call("POST", "/api/rooms", ROOM);
    expect(r.status).toBe(429);
    expect(r.json.error).toBe("too_many_rooms");
    await create({}, "198.51.100.99"); // someone else still can
  });

  it("limits room creations per minute", async () => {
    for (let i = 0; i < 10; i++) {
      const room = await create();
      expect((await call("DELETE", "/api/rooms/" + room.room_id, undefined, { token: room.host_token })).status).toBe(200);
    }
    const r = await call("POST", "/api/rooms", ROOM);
    expect(r.status).toBe(429);
    expect(r.json.error).toBe("too_many_creates");
  });
});

describe("list", () => {
  it("shows public rooms only, filtered, without addresses or tokens", async () => {
    const pub = await create();
    const priv = await create({ name: "Friends only", public: false });
    await create({ name: "Normal run", difficulty: "normal", mod: { id: "none", version: "" } }, "198.51.100.5");
    const all = await list();
    expect(all.map((r) => r.name)).toEqual(["Baal runs", "Normal run"]);
    const text = JSON.stringify(all);
    for (const secret of [pub.host_token, HOST_IP, "198.51.100.5", "addr", "token"]) expect(text).not.toContain(secret);
    expect((await byCode(priv.code)).status).toBe(200); // a private room is found by its code
    expect((await list("?mod=d2r-vr")).map((r) => r.name)).toEqual(["Baal runs"]);
    expect((await list("?difficulty=normal")).map((r) => r.name)).toEqual(["Normal run"]);
    expect(await list("?mod=d2r-pcvr")).toEqual([]);
  });
});

describe("join", () => {
  it("hands out the address and flags another mod", async () => {
    const room = await create({ port: 4000 });
    let r = await join({ code: room.code.toLowerCase() });
    expect(r.status).toBe(200);
    expect(r.json).toMatchObject({ room_id: room.room_id, address: HOST_IP, port: 4000, mod_mismatch: true });
    r = await join({ room_id: room.room_id, mod: { id: "d2r-vr", version: "" } });
    expect(r.json.mod_mismatch).toBe(false);
    expect((await join({ code: "QQQQ" })).status).toBe(404);
    expect((await call("POST", "/api/join", { mod: JOIN_MOD })).status).toBe(422);
  });

  it("checks the password and limits wrong guesses", async () => {
    const room = await create({ password: "hunter2" });
    expect((await byCode(room.code)).json.room.locked).toBe(true);
    expect((await join({ room_id: room.room_id })).status).toBe(403);
    expect((await join({ room_id: room.room_id, password: "hunter2" })).status).toBe(200);
    for (let i = 0; i < 4; i++) {
      const r = await join({ room_id: room.room_id, password: "wrong" });
      expect(r.status).toBe(403);
      expect(r.json.error).toBe("bad_password");
    }
    const r = await join({ room_id: room.room_id, password: "hunter2" });
    expect(r.status).toBe(429);
    expect(r.json.error).toBe("too_many_attempts");
    expect((await join({ room_id: room.room_id, password: "hunter2" }, "198.51.100.77")).status).toBe(200);
  });
});

describe("players", () => {
  it("merges the host roster with what joiners said", async () => {
    const room = await create({ player: { name: "Typed", class: "paladin", level: 80 } });
    // No roster yet: the host's typed character with the room's mod, and joiners who named theirs.
    expect((await list())[0].players).toEqual([
      { name: "Typed", class: "paladin", level: 80, act: null, area: "", vr: null, host: true, mod: ROOM.mod },
    ]);
    await join({ room_id: room.room_id, player: { name: "Bob", class: "sorceress", level: 82 } });
    await join({ room_id: room.room_id, mod: { id: "none", version: "" } });
    let rows = (await list())[0].players;
    expect(rows.map((p: any) => [p.name, p.mod?.id])).toEqual([["Typed", "d2r-vr"], ["Bob", "d2r-pcvr"]]);
    expect((await list())[0].players_count).toBe(2);

    // The host's plugin reports the real roster: it is the truth now.
    const r = await heartbeat(room, {
      vr: true, act: 5, players: [
        { name: "Aria", class: "paladin", level: 85, act: 5, area: "Worldstone Keep Level 2", self: true },
        { name: "Bob", class: "sorceress", level: 82, act: 5, area: "Throne of Destruction" },
        { name: "Eve", class: "druid", level: 79, act: 4, area: "River of Flame" },
      ],
    });
    expect(r.status).toBe(200);
    const view = (await list())[0];
    rows = Object.fromEntries(view.players.map((p: any) => [p.name, p]));
    expect(Object.keys(rows)).toEqual(["Aria", "Bob", "Eve"]);
    expect(rows.Aria).toMatchObject({ host: true, vr: true, act: 5, mod: ROOM.mod });
    expect(rows.Bob).toMatchObject({ host: false, mod: JOIN_MOD, area: "Throne of Destruction" });
    expect(rows.Eve.mod).toBeNull(); // she never named her character when joining
    expect(view).toMatchObject({ players_count: 3, act: 5 });
  });

  it("keeps only one joiner entry per name", async () => {
    const room = await create();
    for (let i = 0; i < 3; i++) await join({ room_id: room.room_id, player: { name: "Bob", class: "sorceress", level: 80 + i } });
    const view = (await list())[0];
    expect(view.players).toHaveLength(1);
    expect(view.players[0].level).toBe(82);
    expect(view.players_count).toBe(2); // Bob and the unnamed host
  });
});

describe("lifetime", () => {
  it("heartbeat keeps the room, needs the token and valid data", async () => {
    const room = await create();
    await age(room.room_id, 150);
    expect((await heartbeat(room)).status).toBe(200);
    await age(room.room_id, 150); // 150 s after the heartbeat: still within 180
    expect((await byCode(room.code)).status).toBe(200);
    expect((await heartbeat(room, {}, "nope")).status).toBe(403);
    expect((await heartbeat(room, { players: [{ name: "", class: "paladin" }] })).status).toBe(422);
    expect((await call("POST", "/api/rooms/nope/heartbeat", {}, { token: room.host_token })).status).toBe(404);
  });

  it("a room without heartbeat disappears everywhere and is deleted on the next create", async () => {
    const room = await create();
    await age(room.room_id, 181);
    expect((await byCode(room.code)).status).toBe(404);
    expect(await list()).toEqual([]);
    expect((await heartbeat(room)).status).toBe(404);
    expect((await join({ room_id: room.room_id })).status).toBe(404);
    expect((await env.DB.prepare("SELECT COUNT(*) AS n FROM rooms").first<any>()).n).toBe(1);
    await create({}, "198.51.100.3");
    expect((await env.DB.prepare("SELECT COUNT(*) AS n FROM rooms").first<any>()).n).toBe(1);
  });

  it("only the host removes the room", async () => {
    const room = await create();
    expect((await call("DELETE", "/api/rooms/" + room.room_id, undefined, { token: "nope" })).status).toBe(403);
    expect((await call("DELETE", "/api/rooms/" + room.room_id, undefined, { token: room.host_token })).status).toBe(200);
    expect((await byCode(room.code)).status).toBe(404);
  });
});

describe("page", () => {
  it("serves the board page and 404s the rest", async () => {
    const page = await exports.default.fetch(new Request("https://rooms.test/"));
    expect(page.headers.get("content-type")).toContain("text/html");
    const html = await page.text();
    expect(html).toContain("<title>D2R Rooms</title>");
    expect(html).not.toContain("${");
    expect((await call("GET", "/healthz")).json).toEqual({ ok: true });
    expect((await call("GET", "/nope")).status).toBe(404);
    expect((await call("PUT", "/api/rooms")).status).toBe(404);
  });
});
