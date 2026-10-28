// One RoomRelay object per room id: the host's socket and up to 7 joiner
// sockets, piped as PROTOCOL.md says. It uses the WebSocket Hibernation API, so
// a host waiting in the lobby with no joiner costs nothing; once a game runs,
// bytes flow all the time and the object stays awake.
//
// The object is created where its first request comes from. The host's socket
// comes first, so the room sits near the host - the game's server.

import { DurableObject } from "cloudflare:workers";
import type { BudgetView } from "./budget";
import { messagesToRequests } from "./cost";
import type { Env } from "./index";

export const VERSION = 1;
export const PIPES = 7;
export const MAX_MESSAGE = 65536;
const OPEN = 1, DATA = 2, CLOSE = 3;
const RATE = 256 * 1024; // bytes a second per pipe and direction
const BURST = 1024 * 1024;
const CLAIM_KEPT_MS = 24 * 3600 * 1000;
const REPORT_EVERY_MS = 60 * 1000;
const STATUS_CACHE_MS = 30 * 1000;

interface Claim {
  secret: string; // SHA-256 hex of the host secret
  join: string; // SHA-256 hex of the join key
}

type Attachment = { role: "host" } | { role: "join"; pipe: number; byHost?: boolean };

class Bucket {
  private tokens = BURST;
  private stamp = Date.now();
  take(n: number): boolean {
    const t = Date.now();
    this.tokens = Math.min(BURST, this.tokens + ((t - this.stamp) / 1000) * RATE);
    this.stamp = t;
    this.tokens -= n;
    return this.tokens >= 0;
  }
}

export function refusal(status: number, error: string): Response {
  return new Response(JSON.stringify({ error }), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });
}

function text(o: unknown): string {
  return JSON.stringify(o);
}

export class RoomRelay extends DurableObject<Env> {
  // In memory only; a hibernated object starts these again from zero.
  private up = new Map<number, Bucket>();
  private down = new Map<number, Bucket>();
  private messages = 0;
  private connections = 0;
  private awakeSince = 0; // ms, while pipes are open
  private awakeMs = 0;
  private budget: { at: number; view: BudgetView } | null = null;

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    // Answered by the runtime without waking the object or billing it.
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('{"t":"ping"}', '{"t":"pong"}'));
  }

  // --- helpers ------------------------------------------------------------

  private host(): WebSocket | null {
    for (const ws of this.ctx.getWebSockets("host")) if (ws.readyState === WebSocket.OPEN) return ws;
    return null;
  }

  private pipe(n: number): WebSocket | null {
    for (const ws of this.ctx.getWebSockets(`p${n}`)) if (ws.readyState === WebSocket.OPEN) return ws;
    return null;
  }

  private openPipes(): number[] {
    const used: number[] = [];
    for (const ws of this.ctx.getWebSockets("join")) {
      const a = ws.deserializeAttachment() as Attachment | null;
      if (a?.role === "join" && ws.readyState === WebSocket.OPEN) used.push(a.pipe);
    }
    return used;
  }

  private bucket(map: Map<number, Bucket>, n: number): Bucket {
    let b = map.get(n);
    if (!b) map.set(n, (b = new Bucket()));
    return b;
  }

  private budgetStub() {
    return this.env.BUDGET.get(this.env.BUDGET.idFromName("budget"));
  }

  private async status(): Promise<BudgetView> {
    const t = Date.now();
    if (this.budget && t - this.budget.at < STATUS_CACHE_MS) return this.budget.view;
    const view = await this.budgetStub().check();
    this.budget = { at: t, view };
    return view;
  }

  private markAwake(): void {
    if (this.openPipes().length > 0) {
      if (!this.awakeSince) this.awakeSince = Date.now();
    } else if (this.awakeSince) {
      this.awakeMs += Date.now() - this.awakeSince;
      this.awakeSince = 0;
    }
  }

  private async report(): Promise<void> {
    const t = Date.now();
    if (this.awakeSince) {
      this.awakeMs += t - this.awakeSince;
      this.awakeSince = t;
    }
    const used = {
      // + this report and the alarm that sent it
      requests: messagesToRequests(this.messages) + this.connections + 2,
      awakeSeconds: this.awakeMs / 1000,
    };
    this.messages = 0;
    this.connections = 0;
    this.awakeMs = 0;
    if (used.requests <= 2 && used.awakeSeconds === 0) return;
    const view = await this.budgetStub().report(used);
    this.budget = { at: t, view };
  }

  /** Host gone and no pipes: report, and keep the claim one more day. */
  private async release(): Promise<void> {
    await this.report();
    const released = Date.now();
    await this.ctx.storage.put("released", released);
    await this.ctx.storage.setAlarm(released + CLAIM_KEPT_MS);
  }

  // --- the upgrade (index.ts checked the formats and the relay key) --------

  async fetch(req: Request): Promise<Response> {
    const side = req.headers.get("x-relay-side");
    return side === "host" ? this.acceptHost(req) : this.acceptJoin(req);
  }

  private async acceptHost(req: Request): Promise<Response> {
    const secret = req.headers.get("x-host-secret-hash")!;
    const join = req.headers.get("x-join-key-hash")!;
    let claim = await this.ctx.storage.get<Claim>("claim");
    if (claim && claim.secret !== secret) return refusal(403, "bad_secret");
    if (!claim && (await this.status()).status !== "active") return refusal(503, "near_limit");
    claim = { secret, join };
    await this.ctx.storage.put("claim", claim);

    // The same host again: the old socket and its pipes go.
    for (const old of this.ctx.getWebSockets("host")) {
      try { old.close(4000, "replaced"); } catch { /* already closing */ }
    }
    this.dropPipes();

    const pair = new WebSocketPair();
    this.ctx.acceptWebSocket(pair[1], ["host"]);
    pair[1].serializeAttachment({ role: "host" } satisfies Attachment);
    pair[1].send(text({ t: "hello", v: VERSION, pipes: PIPES, max_message: MAX_MESSAGE }));
    this.connections++;
    await this.ctx.storage.delete("released");
    await this.ctx.storage.deleteAlarm();
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  private async acceptJoin(req: Request): Promise<Response> {
    const claim = await this.ctx.storage.get<Claim>("claim");
    const host = this.host();
    if (!claim || !host) return refusal(404, "no_room");
    if (claim.join !== req.headers.get("x-join-key-hash")) return refusal(403, "bad_join_key");
    if ((await this.status()).status === "limit") return refusal(503, "over_budget");
    const used = this.openPipes();
    if (used.length >= PIPES) return refusal(409, "room_full");
    let n = 1;
    while (used.includes(n)) n++;

    const pair = new WebSocketPair();
    this.ctx.acceptWebSocket(pair[1], ["join", `p${n}`]);
    pair[1].serializeAttachment({ role: "join", pipe: n } satisfies Attachment);
    this.up.set(n, new Bucket());
    this.down.set(n, new Bucket());
    host.send(new Uint8Array([OPEN, n]));
    pair[1].send(text({ t: "hello", v: VERSION, pipe: n, pipes: PIPES, max_message: MAX_MESSAGE }));
    this.connections++;
    this.markAwake();
    if (!(await this.ctx.storage.getAlarm())) await this.ctx.storage.setAlarm(Date.now() + REPORT_EVERY_MS);
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  // --- messages -------------------------------------------------------------

  async webSocketMessage(ws: WebSocket, msg: string | ArrayBuffer): Promise<void> {
    this.messages++;
    if (typeof msg === "string") return; // pings are answered by the runtime; other text is for later versions
    const a = ws.deserializeAttachment() as Attachment | null;
    if (!a) return;
    const bytes = new Uint8Array(msg);

    if (a.role === "host") {
      if (bytes.length < 2) return;
      const n = bytes[1];
      const target = this.pipe(n);
      if (!target) return;
      if (bytes[0] === DATA) {
        if (!this.bucket(this.down, n).take(bytes.length - 2)) return this.closePipe(target, n, "rate", true);
        target.send(bytes.subarray(2));
      } else if (bytes[0] === CLOSE) {
        this.closePipe(target, n, new TextDecoder().decode(bytes.subarray(2)), false);
      }
      return;
    }

    if (bytes.length > MAX_MESSAGE) {
      ws.close(1009, "too big");
      return;
    }
    if (!this.bucket(this.up, a.pipe).take(bytes.length)) return this.closePipe(ws, a.pipe, "rate", true);
    const host = this.host();
    if (!host) return;
    const frame = new Uint8Array(bytes.length + 2);
    frame[0] = DATA;
    frame[1] = a.pipe;
    frame.set(bytes, 2);
    host.send(frame);
  }

  /** Ends pipe n. tellHost: the relay ended it (rate), so the host hears why. */
  private closePipe(ws: WebSocket, n: number, reason: string, tellHost: boolean): void {
    // The host either asked for this or is told here: webSocketClose stays quiet.
    ws.serializeAttachment({ role: "join", pipe: n, byHost: true } satisfies Attachment);
    if (tellHost) {
      const enc = new TextEncoder().encode(reason);
      const frame = new Uint8Array(enc.length + 2);
      frame[0] = CLOSE;
      frame[1] = n;
      frame.set(enc, 2);
      this.host()?.send(frame);
    }
    try { ws.close(1000, reason.slice(0, 120)); } catch { /* already closing */ }
    this.up.delete(n);
    this.down.delete(n);
  }

  async webSocketClose(ws: WebSocket, code: number, reason: string): Promise<void> {
    const a = ws.deserializeAttachment() as Attachment | null;
    try { ws.close(code === 1005 || code === 1006 ? 1000 : code, reason); } catch { /* already closed */ }
    if (a?.role === "join") {
      if (!a.byHost) this.host()?.send(new Uint8Array([CLOSE, a.pipe]));
      this.up.delete(a.pipe);
      this.down.delete(a.pipe);
    } else if (a?.role === "host" && !this.host()) {
      this.dropPipes();
    }
    this.markAwake();
    if (!this.host() && this.openPipes().length === 0) await this.release();
  }

  /** Closes every joiner with host_gone; their closes send nothing to a host. */
  private dropPipes(): void {
    for (const j of this.ctx.getWebSockets("join")) {
      const ja = j.deserializeAttachment() as Attachment | null;
      if (ja?.role === "join") j.serializeAttachment({ ...ja, byHost: true });
      try { j.close(1001, "host_gone"); } catch { /* already closing */ }
    }
    this.up.clear();
    this.down.clear();
  }

  async webSocketError(ws: WebSocket): Promise<void> {
    await this.webSocketClose(ws, 1011, "error");
  }

  async alarm(): Promise<void> {
    if (this.openPipes().length > 0) {
      await this.report();
      await this.ctx.storage.setAlarm(Date.now() + REPORT_EVERY_MS);
      return;
    }
    if (this.host()) {
      await this.report(); // the host waits in the lobby: nothing to report until a pipe opens
      return;
    }
    const released = await this.ctx.storage.get<number>("released");
    if (released === undefined) return this.release();
    if (Date.now() - released >= CLAIM_KEPT_MS - 1000) {
      // Nobody came back within a day: forget the room.
      await this.report();
      await this.ctx.storage.deleteAll();
    } else {
      await this.ctx.storage.setAlarm(released + CLAIM_KEPT_MS);
    }
  }
}
