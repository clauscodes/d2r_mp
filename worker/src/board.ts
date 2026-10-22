// Rooms in D1. A room lives while its host sends a heartbeat (every 60 s);
// ROOM_TTL seconds without one and it is gone from every answer, and the next
// room creation deletes it for good.

import { CreateRoom, Heartbeat, JoinRoom, Mod, Player, isIPv6 } from "./validate";

export interface Env {
  DB: D1Database;
  // Optional settings (wrangler vars / secrets); defaults below.
  ADDR_SALT?: string;
  ROOM_TTL?: string;
  MAX_ROOMS?: string;
  MAX_ROOMS_PER_ADDR?: string;
  CREATES_PER_MINUTE?: string;
  JOIN_FAIL_LIMIT?: string;
  JOIN_FAIL_WINDOW?: string;
}

export const HEARTBEAT_EVERY = 60;
// How long a joiner without a host roster stays listed after joining.
const JOINER_SHOWN = 30 * 60;
const JOINERS_KEPT = 16;
const CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const PBKDF2_ROUNDS = 2000; // the free plan gives 10 ms of CPU per request

export class Refused extends Error {
  constructor(public status: number, public error: string) {
    super(error);
  }
}

function setting(env: Env, key: keyof Env, def: number): number {
  const raw = env[key];
  const n = typeof raw === "string" ? Number(raw) : NaN;
  return Number.isFinite(n) && n > 0 ? n : def;
}

export const ttl = (env: Env) => setting(env, "ROOM_TTL", 180);

export const now = () => Math.floor(Date.now() / 1000);

// --- crypto -------------------------------------------------------------------

function b64url(bytes: Uint8Array): string {
  let s = "";
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export function randomToken(n = 24): string {
  return b64url(crypto.getRandomValues(new Uint8Array(n)));
}

export async function sha256(text: string): Promise<string> {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function pbkdf2(password: string, salt: string): Promise<string> {
  const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(password), "PBKDF2", false, ["deriveBits"]);
  const bits = await crypto.subtle.deriveBits(
    { name: "PBKDF2", hash: "SHA-256", salt: new TextEncoder().encode(salt), iterations: PBKDF2_ROUNDS },
    key,
    256,
  );
  return b64url(new Uint8Array(bits));
}

async function hashPassword(password: string): Promise<string> {
  const salt = randomToken(12);
  return `${salt}:${await pbkdf2(password, salt)}`;
}

async function passwordOk(stored: string | null, password: string): Promise<boolean> {
  if (!stored) return true;
  const [salt, hash] = stored.split(":");
  return (await pbkdf2(password, salt)) === hash;
}

/** Addresses used for limits are kept only as a keyed hash that changes daily. */
export function addrHash(env: Env, ip: string): Promise<string> {
  const day = Math.floor(Date.now() / 86_400_000);
  return sha256(`${env.ADDR_SALT ?? "d2r-rooms"}:${day}:${ip}`);
}

// --- rows -------------------------------------------------------------------

interface Joiner {
  name: string | null;
  mod: Mod | null;
  player: Player | null;
  at: number;
}

interface RoomData {
  host_player: Player | null;
  roster: Player[];
  vr: boolean | null;
  joiners: Joiner[];
}

interface RoomRow {
  room_id: string;
  code: string;
  token_hash: string;
  addr_hash: string;
  address: string;
  port: number;
  name: string;
  mod_id: string;
  mod_version: string;
  difficulty: string;
  mode: string;
  act: number | null;
  max_players: number;
  public: number;
  password_hash: string | null;
  data: string;
  created_at: number;
  last_seen: number;
}

function roomData(row: RoomRow): RoomData {
  return { host_player: null, roster: [], vr: null, joiners: [], ...JSON.parse(row.data) };
}

interface PlayerRow {
  name: string;
  class: string;
  level: number | null;
  act: number | null;
  area: string;
  vr: boolean | null;
  host: boolean;
  mod: Mod | null;
}

/**
 * The public player rows. With a host roster (the host's plugin sees everyone
 * in the game) the roster is the truth: the host's own entry carries the room's
 * mod, the others the mod of the joiner who gave the same name. Without one,
 * the host's typed character and recent joiners who named theirs are shown.
 */
function players(row: RoomRow, data: RoomData, t: number): PlayerRow[] {
  const roomMod = { id: row.mod_id, version: row.mod_version };
  const joinerMod = new Map<string, Mod | null>();
  for (const j of data.joiners) if (j.name) joinerMod.set(j.name, j.mod);
  const asRow = (p: Player, mod: Mod | null, host: boolean): PlayerRow => ({
    name: p.name, class: p.class, level: p.level, act: p.act, area: p.area, vr: p.vr, host, mod,
  });

  const out = new Map<string, PlayerRow>();
  if (data.roster.length) {
    for (const p of data.roster) {
      if (out.has(p.name)) continue;
      const r = p.self ? asRow(p, roomMod, true) : asRow(p, joinerMod.get(p.name) ?? null, false);
      if (p.self && r.vr === null) r.vr = data.vr;
      out.set(p.name, r);
    }
    return [...out.values()];
  }
  if (data.host_player) out.set(data.host_player.name, { ...asRow(data.host_player, roomMod, true), vr: data.host_player.vr ?? data.vr });
  for (const j of data.joiners) {
    if (j.player && !out.has(j.player.name) && t - j.at < JOINER_SHOWN) out.set(j.player.name, asRow(j.player, j.mod, false));
  }
  return [...out.values()];
}

function view(row: RoomRow, t: number) {
  const data = roomData(row);
  const list = players(row, data, t);
  return {
    room_id: row.room_id,
    code: row.code,
    name: row.name,
    mod: { id: row.mod_id, version: row.mod_version },
    difficulty: row.difficulty,
    mode: row.mode,
    act: row.act,
    max_players: row.max_players,
    // The host plays even when nothing named their character.
    players_count: list.length + (list.some((p) => p.host) ? 0 : 1),
    locked: row.password_hash !== null,
    age: t - row.created_at,
    players: list,
  };
}

// --- board ------------------------------------------------------------------

export class Board {
  constructor(private env: Env) {}

  private get db() {
    return this.env.DB;
  }

  private cutoff(): number {
    return now() - ttl(this.env);
  }

  private async attempts(key: string, window: number): Promise<number> {
    const r = await this.db.prepare("SELECT COUNT(*) AS n FROM attempts WHERE key = ? AND at > ?").bind(key, now() - window).first<{ n: number }>();
    return r?.n ?? 0;
  }

  private async live(where: string, ...args: unknown[]): Promise<RoomRow | null> {
    return this.db.prepare(`SELECT * FROM rooms WHERE ${where} AND last_seen >= ?`).bind(...args, this.cutoff()).first<RoomRow>();
  }

  async create(req: CreateRoom, ip: string) {
    const t = now();
    const ah = await addrHash(this.env, ip);
    // Expired rooms and old attempts go here, the one place that writes anyway.
    await this.db.batch([
      this.db.prepare("DELETE FROM rooms WHERE last_seen < ?").bind(this.cutoff()),
      this.db.prepare("DELETE FROM attempts WHERE at < ?").bind(t - 3600),
    ]);
    const counts = await this.db
      .prepare("SELECT COUNT(*) AS total, SUM(addr_hash = ?) AS mine FROM rooms")
      .bind(ah)
      .first<{ total: number; mine: number | null }>();
    if ((counts?.total ?? 0) >= setting(this.env, "MAX_ROOMS", 500)) throw new Refused(503, "server_full");
    if ((counts?.mine ?? 0) >= setting(this.env, "MAX_ROOMS_PER_ADDR", 3)) throw new Refused(429, "too_many_rooms");
    if ((await this.attempts(`create:${ah}`, 60)) >= setting(this.env, "CREATES_PER_MINUTE", 10))
      throw new Refused(429, "too_many_creates");

    const address = req.address ?? ip;
    const token = randomToken();
    const data: RoomData = { host_player: req.player, roster: [], vr: req.player?.vr ?? null, joiners: [] };
    const password = req.password ? await hashPassword(req.password) : null;
    for (let attempt = 0; attempt < 20; attempt++) {
      const code = Array.from(crypto.getRandomValues(new Uint8Array(4)), (b) => CODE_ALPHABET[b % CODE_ALPHABET.length]).join("");
      const room_id = randomToken(9);
      try {
        await this.db.batch([
          this.db
            .prepare(
              `INSERT INTO rooms (room_id, code, token_hash, addr_hash, address, port, name, mod_id, mod_version,
                 difficulty, mode, act, max_players, public, password_hash, data, created_at, last_seen)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
            )
            .bind(room_id, code, await sha256(token), ah, address, req.port, req.name, req.mod.id, req.mod.version,
              req.difficulty, req.mode, req.act, req.max_players, req.public ? 1 : 0, password, JSON.stringify(data), t, t),
          this.db.prepare("INSERT INTO attempts (key, at) VALUES (?, ?)").bind(`create:${ah}`, t),
        ]);
      } catch (e) {
        if (String(e).includes("UNIQUE")) continue; // code taken: draw another
        throw e;
      }
      return {
        room_id, code, host_token: token, address, port: req.port,
        address_ipv6: isIPv6(address), heartbeat_every: HEARTBEAT_EVERY, ttl: ttl(this.env),
      };
    }
    throw new Refused(503, "no_free_code");
  }

  private async owned(room_id: string, token: string): Promise<RoomRow> {
    const row = await this.live("room_id = ?", room_id);
    if (!row) throw new Refused(404, "no_room");
    if ((await sha256(token)) !== row.token_hash) throw new Refused(403, "bad_token");
    return row;
  }

  async heartbeat(room_id: string, token: string, hb: Heartbeat) {
    const row = await this.owned(room_id, token);
    const data = roomData(row);
    if (hb.players) data.roster = hb.players;
    if (hb.vr !== null) data.vr = hb.vr;
    await this.db
      .prepare("UPDATE rooms SET last_seen = ?, data = ?, difficulty = ?, mode = ?, act = ? WHERE room_id = ?")
      .bind(now(), JSON.stringify(data), hb.difficulty ?? row.difficulty, hb.mode ?? row.mode, hb.act ?? row.act, room_id)
      .run();
    return { ok: true, heartbeat_every: HEARTBEAT_EVERY };
  }

  async remove(room_id: string, token: string) {
    await this.owned(room_id, token);
    await this.db.prepare("DELETE FROM rooms WHERE room_id = ?").bind(room_id).run();
    return { ok: true };
  }

  async list(mod: string | null, difficulty: string | null) {
    const t = now();
    const { results } = await this.db.prepare("SELECT * FROM rooms WHERE public = 1 AND last_seen >= ?").bind(this.cutoff()).all<RoomRow>();
    return results
      .filter((r) => (!mod || r.mod_id === mod) && (!difficulty || r.difficulty === difficulty))
      .map((r) => view(r, t))
      .sort((a, b) => a.name.toLowerCase().localeCompare(b.name.toLowerCase()));
  }

  async byCode(code: string) {
    const row = await this.live("code = ?", code.trim().toUpperCase());
    if (!row) throw new Refused(404, "no_room");
    return view(row, now());
  }

  async join(req: JoinRoom, ip: string) {
    const row = req.room_id ? await this.live("room_id = ?", req.room_id) : await this.live("code = ?", req.code!.trim().toUpperCase());
    if (!row) throw new Refused(404, "no_room");
    const key = `pw:${row.room_id}:${await addrHash(this.env, ip)}`;
    const window = setting(this.env, "JOIN_FAIL_WINDOW", 60);
    if ((await this.attempts(key, window)) >= setting(this.env, "JOIN_FAIL_LIMIT", 5)) throw new Refused(429, "too_many_attempts");
    if (!(await passwordOk(row.password_hash, req.password))) {
      await this.db.prepare("INSERT INTO attempts (key, at) VALUES (?, ?)").bind(key, now()).run();
      throw new Refused(403, "bad_password");
    }
    if (req.mod || req.player) {
      const data = roomData(row);
      const name = req.player?.name ?? null;
      data.joiners = data.joiners.filter((j) => !name || j.name !== name);
      data.joiners.push({ name, mod: req.mod, player: req.player, at: now() });
      data.joiners = data.joiners.slice(-JOINERS_KEPT);
      await this.db.prepare("UPDATE rooms SET data = ? WHERE room_id = ?").bind(JSON.stringify(data), row.room_id).run();
    }
    return {
      room_id: row.room_id,
      address: row.address,
      port: row.port,
      address_ipv6: isIPv6(row.address),
      mod_mismatch: req.mod !== null && req.mod.id !== row.mod_id,
    };
  }
}
