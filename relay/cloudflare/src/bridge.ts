// The outgoing bridge (PROTOCOL.md §6): a joiner whose route to a host is
// blocked (its ISP drops the host's address) reaches the host's own open port
// through the relay. One Bridge object per joiner's connection: it opens a TCP
// socket to the host's address before it takes the WebSocket, then pipes the
// bytes both ways. Only public addresses and the game's ports, so it is no
// open proxy; its messages and its time awake go into the same Budget as the
// rooms'.

import { connect } from "cloudflare:sockets";
import { DurableObject } from "cloudflare:workers";
import { messagesToRequests } from "./cost";
import type { Env } from "./index";
import { Bucket, MAX_MESSAGE, VERSION, refusal } from "./room";

const CONNECT_TIMEOUT_MS = 8000;
const REPORT_EVERY_MS = 60 * 1000;

// --- what may be bridged to (index.ts checks it before the object) -----------

function ipv4(text: string): number[] | null {
  const parts = text.split(".");
  if (parts.length !== 4) return null;
  const out: number[] = [];
  for (const p of parts) {
    if (!/^(0|[1-9][0-9]{0,2})$/.test(p)) return null;
    const n = Number(p);
    if (n > 255) return null;
    out.push(n);
  }
  return out;
}

function publicIpv4(b: number[]): boolean {
  const [a, c] = b;
  if (a === 0 || a === 10 || a === 127 || a >= 224) return false; // this net, private, loopback, multicast and reserved
  if (a === 100 && c >= 64 && c < 128) return false; // carrier-grade NAT
  if (a === 169 && c === 254) return false; // link-local
  if (a === 172 && c >= 16 && c < 32) return false;
  if (a === 192 && c === 168) return false;
  if (a === 192 && c === 0 && b[2] === 0) return false; // IETF protocol assignments
  if (a === 198 && (c === 18 || c === 19)) return false; // benchmarking
  return true;
}

/** Eight 16-bit groups, or null when it is no IPv6 literal. */
function ipv6(text: string): number[] | null {
  if (!/^[0-9a-fA-F:]{2,39}$/.test(text) || text.includes(":::")) return null;
  const halves = text.split("::");
  if (halves.length > 2) return null;
  const groups = (s: string) => (s === "" ? [] : s.split(":"));
  const head = groups(halves[0]), tail = halves.length === 2 ? groups(halves[1]) : [];
  const missing = 8 - head.length - tail.length;
  if (halves.length === 1 ? missing !== 0 : missing < 1) return null;
  const all = [...head, ...Array(halves.length === 2 ? missing : 0).fill("0"), ...tail];
  if (all.some((g) => !/^[0-9a-fA-F]{1,4}$/.test(g))) return null;
  return all.map((g) => parseInt(g, 16));
}

function publicIpv6(g: number[]): boolean {
  if (g.slice(0, 7).every((x) => x === 0)) return false; // :: and ::1
  if (g.slice(0, 5).every((x) => x === 0) && g[5] === 0xffff) return false; // IPv4-mapped: the IPv4 form is asked for
  if ((g[0] & 0xfe00) === 0xfc00) return false; // unique local
  if ((g[0] & 0xffc0) === 0xfe80) return false; // link-local
  if ((g[0] & 0xff00) === 0xff00) return false; // multicast
  if (g[0] === 0x2001 && g[1] === 0x0db8) return false; // documentation
  return true;
}

export interface BridgeTarget {
  hostname: string;
  port: number;
}

/** The address and port a joiner asks for, or the refusal's error code. */
export function bridgeTarget(address: string, port: string, ports: number[], allowPrivate = false): BridgeTarget | string {
  if (!/^[0-9]{1,5}$/.test(port) || !ports.includes(Number(port))) return "bad_port";
  const v4 = ipv4(address);
  if (v4) return publicIpv4(v4) || allowPrivate ? { hostname: address, port: Number(port) } : "bad_address";
  const v6 = ipv6(address);
  if (v6) return publicIpv6(v6) || allowPrivate ? { hostname: address.toLowerCase(), port: Number(port) } : "bad_address";
  return "bad_address";
}

export function bridgePorts(raw: string | undefined): number[] {
  const out = (raw ?? "4000")
    .split(",")
    .map((p) => Number(p.trim()))
    .filter((p) => Number.isInteger(p) && p > 0 && p < 65536);
  return out.length ? out : [4000];
}

// --- the object ---------------------------------------------------------------

export class Bridge extends DurableObject<Env> {
  private messages = 0;
  private awakeSince = 0;
  private reported = 0;
  private closed = false;
  private socket: Socket | null = null;
  private ws: WebSocket | null = null;

  async fetch(req: Request): Promise<Response> {
    const hostname = req.headers.get("x-bridge-host")!;
    const port = Number(req.headers.get("x-bridge-port"));
    // The host first: a joiner gets 502 at once when it cannot be reached
    // from here either, and nothing is billed for a pipe.
    let socket: Socket;
    try {
      socket = connect({ hostname, port }, { allowHalfOpen: false });
      let timer: ReturnType<typeof setTimeout> | undefined;
      const late = new Promise<never>((_, reject) => { timer = setTimeout(() => reject(new Error("timeout")), CONNECT_TIMEOUT_MS); });
      try {
        await Promise.race([socket.opened, late]);
      } finally {
        if (timer !== undefined) clearTimeout(timer);
      }
    } catch {
      await this.budgetStub().report({ requests: 2, awakeSeconds: CONNECT_TIMEOUT_MS / 1000 });
      return refusal(502, "unreachable");
    }

    const pair = new WebSocketPair();
    const ws = pair[1];
    ws.accept();
    this.socket = socket;
    this.ws = ws;
    this.awakeSince = Date.now();
    this.reported = Date.now();
    ws.send(JSON.stringify({ t: "hello", v: VERSION, bridge: true, max_message: MAX_MESSAGE }));

    const writer = socket.writable.getWriter();
    const up = new Bucket();
    // Joiner -> host in order: a message may come as a Blob (read one by one).
    let writes = Promise.resolve();
    ws.addEventListener("message", (e) => {
      this.messages++;
      const data = e.data as string | ArrayBuffer | Blob;
      if (typeof data === "string") {
        if (data === '{"t":"ping"}') ws.send('{"t":"pong"}');
        return;
      }
      const size = data instanceof Blob ? data.size : data.byteLength;
      if (size > MAX_MESSAGE) return this.end(1009, "too big");
      if (!up.take(size)) return this.end(1000, "rate");
      if (!size) return;
      writes = writes
        .then(async () => writer.write(new Uint8Array(data instanceof Blob ? await data.arrayBuffer() : data)))
        .catch(() => this.end(1000, "host closed"));
    });
    ws.addEventListener("close", () => this.end(1000, "joiner closed"));
    ws.addEventListener("error", () => this.end(1011, "error"));

    // Host -> joiner, here until the host's side ends.
    this.ctx.waitUntil(this.pump(socket));
    await this.ctx.storage.setAlarm(Date.now() + REPORT_EVERY_MS);
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  private async pump(socket: Socket): Promise<void> {
    const down = new Bucket();
    const reader = socket.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done || this.closed) break;
        if (!value?.length) continue;
        if (!down.take(value.length)) return this.end(1000, "rate");
        // A WebSocket message stays within max_message.
        for (let at = 0; at < value.length; at += MAX_MESSAGE) this.ws?.send(value.subarray(at, at + MAX_MESSAGE));
      }
      this.end(1000, "host closed");
    } catch {
      this.end(1000, "host closed");
    }
  }

  private budgetStub() {
    return this.env.BUDGET.get(this.env.BUDGET.idFromName("budget"));
  }

  private async report(): Promise<void> {
    const t = Date.now();
    const awake = this.awakeSince ? (t - Math.max(this.awakeSince, this.reported)) / 1000 : 0;
    const used = { requests: messagesToRequests(this.messages) + 2, awakeSeconds: awake };
    this.messages = 0;
    this.reported = t;
    await this.budgetStub().report(used);
  }

  private end(code: number, reason: string): void {
    if (this.closed) return;
    this.closed = true;
    try { this.ws?.close(code, reason); } catch { /* already closing */ }
    try { this.socket?.close(); } catch { /* already closed */ }
    this.ctx.waitUntil(this.report().then(() => this.ctx.storage.deleteAll()));
  }

  async alarm(): Promise<void> {
    if (this.closed || !this.ws) return;
    await this.report();
    await this.ctx.storage.setAlarm(Date.now() + REPORT_EVERY_MS);
  }
}
