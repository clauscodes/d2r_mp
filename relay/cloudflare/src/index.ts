// D2R Rooms relay on Cloudflare: relay/PROTOCOL.md v1. The Worker checks a
// request's form and the relay key, then hands the socket to the room's
// Durable Object, which pipes the bytes. Secrets reach the object only as
// SHA-256 hashes.

import { Budget, BudgetView } from "./budget";
import { MAX_MESSAGE, PIPES, RoomRelay, VERSION, refusal } from "./room";

export { Budget, RoomRelay };

export interface Env {
  ROOMS: DurableObjectNamespace<RoomRelay>;
  BUDGET: DurableObjectNamespace<Budget>;
  RELAY_NAME?: string;
  /** Secret: comma-separated keys a host must send. Unset = open to every host. */
  RELAY_KEYS?: string;
  PLAN?: string;
  BUDGET_USD?: string;
  PLAN_BASE_USD?: string;
  NEAR_LIMIT?: string;
  FREE_SHARE?: string;
  RELAY_OPEN?: string;
}

const ROOM_PATH = /^\/v1\/rooms\/([A-Za-z0-9_-]{16,64})\/(host|join)$/;
const TOKEN = /^[A-Za-z0-9_-]{16,128}$/;
const HEX64 = /^[0-9a-f]{64}$/;
const HEALTH_CACHE_MS = 30 * 1000;

// Per isolate: /healthz asks the Budget object at most every 30 s.
let health: { at: number; view: BudgetView } | null = null;

async function sha256(text: string): Promise<string> {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

/** Compares the SHA-256 of both, so the time taken says nothing about the key. */
async function sameKey(given: string, keys: string[]): Promise<boolean> {
  const g = await sha256(given);
  let ok = false;
  for (const k of keys) if ((await sha256(k)) === g) ok = true;
  return ok;
}

async function budgetView(env: Env): Promise<BudgetView> {
  const t = Date.now();
  if (health && t - health.at < HEALTH_CACHE_MS) return health.view;
  const view = await env.BUDGET.get(env.BUDGET.idFromName("budget")).check();
  health = { at: t, view };
  return view;
}

async function route(req: Request, env: Env): Promise<Response> {
  const url = new URL(req.url);
  const path = url.pathname.replace(/\/+$/, "") || "/";

  if (path === "/healthz") {
    const view = await budgetView(env);
    const body = {
      ok: true, relay: VERSION, name: env.RELAY_NAME ?? "D2R Rooms relay",
      status: view.status, open: view.status !== "limit", usage: view.usage, pipes: PIPES, max_message: MAX_MESSAGE,
    };
    return new Response(JSON.stringify(body), {
      headers: { "content-type": "application/json", "cache-control": "no-store", "access-control-allow-origin": "*" },
    });
  }

  const m = path.match(ROOM_PATH);
  if (!m) return refusal(404, "not_found");
  if ((req.headers.get("upgrade") ?? "").toLowerCase() !== "websocket") return refusal(426, "upgrade_required");
  const [, roomId, side] = m;
  const h = req.headers;
  const fwd = new Headers({ upgrade: "websocket", "x-relay-side": side });

  if (side === "host") {
    const keys = (env.RELAY_KEYS ?? "").split(",").map((k) => k.trim()).filter(Boolean);
    if (keys.length && !(await sameKey(h.get("x-relay-key") ?? "", keys))) return refusal(401, "relay_key");
    const secret = h.get("x-host-secret") ?? "";
    const joinHash = h.get("x-join-key-hash") ?? "";
    if (!TOKEN.test(secret) || !HEX64.test(joinHash)) return refusal(400, "bad_request");
    fwd.set("x-host-secret-hash", await sha256(secret));
    fwd.set("x-join-key-hash", joinHash);
  } else {
    const key = h.get("x-join-key") ?? "";
    if (!TOKEN.test(key)) return refusal(400, "bad_request");
    fwd.set("x-join-key-hash", await sha256(key));
  }

  const room = env.ROOMS.get(env.ROOMS.idFromName(roomId));
  return room.fetch(new Request(req.url, { headers: fwd }));
}

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    try {
      return await route(req, env);
    } catch {
      return refusal(500, "server_error");
    }
  },
} satisfies ExportedHandler<Env>;
