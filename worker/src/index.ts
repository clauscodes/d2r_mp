// D2R Rooms: a board of D2R TCP/IP games. Hosts publish a room, players find
// it and connect their game straight to the host's address. No game traffic
// passes through here.

import { Board, Env, Refused } from "./board";
import { PAGE } from "./page";
import { Invalid, parseCreate, parseHeartbeat, parseJoin } from "./validate";

const V = 1;

function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" },
  });
}

function error(status: number, code: string, extra: Record<string, unknown> = {}): Response {
  return json({ v: V, error: code, ...extra }, status);
}

// Cloudflare sets CF-Connecting-IP on every request that reaches a Worker.
function clientIp(req: Request): string {
  return req.headers.get("cf-connecting-ip") ?? "0.0.0.0";
}

function bearer(req: Request): string {
  return (req.headers.get("authorization") ?? "").replace(/^Bearer\s+/i, "").trim();
}

async function body(req: Request): Promise<unknown> {
  const text = await req.text();
  if (text.length > 16 * 1024) throw new Invalid("body", "too large");
  try {
    return text ? JSON.parse(text) : {};
  } catch {
    throw new Invalid("body", "not JSON");
  }
}

async function route(req: Request, env: Env): Promise<Response> {
  const url = new URL(req.url);
  const path = url.pathname.replace(/\/+$/, "") || "/";
  const board = new Board(env);
  const m = req.method;

  if (m === "GET" && path === "/")
    return new Response(PAGE, { headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-cache" } });
  if (m === "GET" && path === "/healthz") return json({ ok: true });

  if (path === "/api/rooms") {
    if (m === "GET")
      return json({ v: V, rooms: await board.list(url.searchParams.get("mod"), url.searchParams.get("difficulty")) });
    if (m === "POST") return json({ v: V, ...(await board.create(parseCreate(await body(req)), clientIp(req))) });
  }

  const byCode = path.match(/^\/api\/rooms\/by-code\/([A-Za-z0-9]{1,8})$/);
  if (byCode && m === "GET") return json({ v: V, room: await board.byCode(byCode[1]) });

  const hb = path.match(/^\/api\/rooms\/([A-Za-z0-9_-]{1,32})\/heartbeat$/);
  if (hb && m === "POST") return json({ v: V, ...(await board.heartbeat(hb[1], bearer(req), parseHeartbeat(await body(req)))) });

  const one = path.match(/^\/api\/rooms\/([A-Za-z0-9_-]{1,32})$/);
  if (one && m === "DELETE") return json({ v: V, ...(await board.remove(one[1], bearer(req))) });

  if (path === "/api/join" && m === "POST") return json({ v: V, ...(await board.join(parseJoin(await body(req)), clientIp(req))) });

  return error(404, "not_found");
}

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    try {
      return await route(req, env);
    } catch (e) {
      // Only where and what - never the input, which may hold a password.
      if (e instanceof Invalid) return error(422, "invalid", { detail: [{ loc: e.loc, msg: e.message }] });
      if (e instanceof Refused) return error(e.status, e.error);
      return error(500, "server_error");
    }
  },
} satisfies ExportedHandler<Env>;
