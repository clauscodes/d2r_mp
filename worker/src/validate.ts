// Request bodies. Everything a client sends that other players will see goes
// through cleanText: control and format characters are dropped and the length
// is capped, so a room or character name cannot break someone else's list.

export const NAME_MAX = 32;
export const AREA_MAX = 40;

export interface Mod {
  id: string;
  version: string;
}

export interface Player {
  name: string;
  class: string;
  level: number | null;
  act: number | null;
  area: string;
  vr: boolean | null;
  self: boolean;
}

/** The relay a host uses when its own port cannot be reached (relay/PROTOCOL.md). */
export interface Relay {
  url: string;
  room: string;
  key: string;
}

export interface CreateRoom {
  name: string;
  mod: Mod;
  difficulty: string;
  mode: string;
  act: number | null;
  max_players: number;
  public: boolean;
  password: string;
  address: string | null;
  port: number;
  player: Player | null;
  relay: Relay | null;
}

export interface Heartbeat {
  players: Player[] | null;
  /** undefined = unchanged, null = the host stopped using a relay. */
  relay: Relay | null | undefined;
  difficulty: string | null;
  mode: string | null;
  act: number | null;
  vr: boolean | null;
}

export interface JoinRoom {
  room_id: string | null;
  code: string | null;
  password: string;
  mod: Mod | null;
  player: Player | null;
}

export class Invalid extends Error {
  constructor(public loc: string, msg: string) {
    super(msg);
  }
}

const DIFFICULTIES = ["normal", "nightmare", "hell"];
const MODES = ["softcore", "hardcore"];

export function cleanText(value: unknown, limit: number): string {
  if (typeof value !== "string") return "";
  const kept = value.replace(/\p{C}/gu, " ").split(/\s+/).filter(Boolean).join(" ");
  return Array.from(kept).slice(0, limit).join("");
}

function obj(v: unknown, loc: string): Record<string, unknown> {
  if (v === null || typeof v !== "object" || Array.isArray(v)) throw new Invalid(loc, "must be an object");
  return v as Record<string, unknown>;
}

function str(v: unknown, loc: string, max: number, def = ""): string {
  if (v === undefined || v === null) return def;
  if (typeof v !== "string") throw new Invalid(loc, "must be a string");
  if (v.length > max) throw new Invalid(loc, `longer than ${max}`);
  return v;
}

function int(v: unknown, loc: string, min: number, max: number): number | null {
  if (v === undefined || v === null) return null;
  if (typeof v !== "number" || !Number.isInteger(v) || v < min || v > max)
    throw new Invalid(loc, `must be a whole number ${min}..${max}`);
  return v;
}

function bool(v: unknown, loc: string): boolean | null {
  if (v === undefined || v === null) return null;
  if (typeof v !== "boolean") throw new Invalid(loc, "must be true or false");
  return v;
}

function oneOf(v: unknown, loc: string, allowed: string[]): string | null {
  if (v === undefined || v === null) return null;
  if (typeof v !== "string" || !allowed.includes(v)) throw new Invalid(loc, `must be one of ${allowed.join(", ")}`);
  return v;
}

function name(v: unknown, loc: string): string {
  const s = cleanText(str(v, loc, 200), NAME_MAX);
  if (!s) throw new Invalid(loc, "empty");
  return s;
}

export function parseMod(v: unknown, loc = "mod"): Mod {
  const o = obj(v, loc);
  const id = str(o.id, `${loc}.id`, 32);
  if (!/^[a-z0-9][a-z0-9-]{0,31}$/.test(id)) throw new Invalid(`${loc}.id`, "lower-case letters, digits and -");
  const version = str(o.version, `${loc}.version`, 24);
  if (!/^[0-9A-Za-z.+-]*$/.test(version)) throw new Invalid(`${loc}.version`, "letters, digits and . + -");
  return { id, version };
}

export function parsePlayer(v: unknown, loc = "player"): Player {
  const o = obj(v, loc);
  // A class name rather than a fixed list, so a class the game adds later does
  // not make the whole roster invalid.
  const cls = str(o.class, `${loc}.class`, 16, "unknown");
  if (!/^[a-z]{1,16}$/.test(cls)) throw new Invalid(`${loc}.class`, "lower-case letters");
  return {
    name: name(o.name, `${loc}.name`),
    class: cls,
    level: int(o.level, `${loc}.level`, 1, 99),
    act: int(o.act, `${loc}.act`, 1, 5),
    area: cleanText(str(o.area, `${loc}.area`, 200), AREA_MAX),
    vr: bool(o.vr, `${loc}.vr`),
    self: bool(o.self, `${loc}.self`) ?? false,
  };
}

const IPV4 = /^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$/;
const IPV6 = /^[0-9A-Fa-f:.]{2,45}$/;
const HOSTNAME = /^(?=.{1,64}$)[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$/;

export function isIPv6(address: string): boolean {
  return address.includes(":");
}

function address(v: unknown, loc: string): string | null {
  if (v === undefined || v === null || v === "") return null;
  const s = str(v, loc, 64).trim();
  if (IPV4.test(s) || (isIPv6(s) && IPV6.test(s)) || (!IPV4.test(s) && HOSTNAME.test(s) && /[A-Za-z]/.test(s)))
    return s;
  throw new Invalid(loc, "an IPv4/IPv6 address or a host name");
}

const RELAY_URL = /^wss?:\/\/[A-Za-z0-9.-]{1,120}(:\d{1,5})?(\/[A-Za-z0-9._~\/-]{0,60})?$/;
const RELAY_TOKEN = /^[A-Za-z0-9_-]{16,128}$/;

export function parseRelay(v: unknown, loc = "relay"): Relay | null {
  if (v === undefined || v === null) return null;
  const o = obj(v, loc);
  const url = str(o.url, `${loc}.url`, 200).replace(/\/+$/, "");
  if (!RELAY_URL.test(url)) throw new Invalid(`${loc}.url`, "a ws:// or wss:// address");
  const room = str(o.room, `${loc}.room`, 64);
  if (!/^[A-Za-z0-9_-]{16,64}$/.test(room)) throw new Invalid(`${loc}.room`, "16-64 letters, digits, _ and -");
  const key = str(o.key, `${loc}.key`, 128);
  if (!RELAY_TOKEN.test(key)) throw new Invalid(`${loc}.key`, "16-128 letters, digits, _ and -");
  return { url, room, key };
}

export function parseCreate(body: unknown): CreateRoom {
  const o = obj(body, "body");
  return {
    name: name(o.name, "name"),
    mod: parseMod(o.mod),
    difficulty: oneOf(o.difficulty, "difficulty", DIFFICULTIES) ?? "normal",
    mode: oneOf(o.mode, "mode", MODES) ?? "softcore",
    act: int(o.act, "act", 1, 5),
    max_players: int(o.max_players, "max_players", 2, 8) ?? 8,
    public: bool(o.public, "public") ?? true,
    password: str(o.password, "password", 64),
    address: address(o.address, "address"),
    port: int(o.port, "port", 1, 65535) ?? 4000,
    player: o.player === undefined || o.player === null ? null : parsePlayer(o.player),
    relay: parseRelay(o.relay),
  };
}

export function parseHeartbeat(body: unknown): Heartbeat {
  const o = obj(body ?? {}, "body");
  let players: Player[] | null = null;
  if (o.players !== undefined && o.players !== null) {
    if (!Array.isArray(o.players)) throw new Invalid("players", "must be a list");
    if (o.players.length > 8) throw new Invalid("players", "at most 8");
    players = o.players.map((p, i) => parsePlayer(p, `players.${i}`));
  }
  return {
    players,
    relay: o.relay === undefined ? undefined : parseRelay(o.relay),
    difficulty: oneOf(o.difficulty, "difficulty", DIFFICULTIES),
    mode: oneOf(o.mode, "mode", MODES),
    act: int(o.act, "act", 1, 5),
    vr: bool(o.vr, "vr"),
  };
}

export function parseJoin(body: unknown): JoinRoom {
  const o = obj(body, "body");
  const room_id = str(o.room_id, "room_id", 32) || null;
  const code = str(o.code, "code", 8) || null;
  if (!room_id && !code) throw new Invalid("room_id", "room_id or code is needed");
  return {
    room_id,
    code,
    password: str(o.password, "password", 64),
    mod: o.mod === undefined || o.mod === null ? null : parseMod(o.mod),
    player: o.player === undefined || o.player === null ? null : parsePlayer(o.player),
  };
}
