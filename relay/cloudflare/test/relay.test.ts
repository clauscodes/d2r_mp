// The cost model and one round trip inside the Workers runtime. The full
// protocol is checked by ../conformance.py against `wrangler dev`.

import { exports } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import { Settings, paidUsd, settings, status, usage } from "../src/cost";

const PAID: Settings = { plan: "paid", budgetUsd: 10, planBaseUsd: 5, nearLimit: 0.8, freeShare: 0.9, open: true };
const ZERO = { requests: 0, awakeSeconds: 0 };

describe("cost", () => {
  it("is the plan's $5 inside the included usage", () => {
    expect(paidUsd({ requests: 1_000_000, awakeSeconds: 3_200_000 }, PAID)).toBe(5);
  });

  it("adds requests and GB-s past the included usage", () => {
    // 2 500 joiner-hours (22.5 M requests) and 1 500 room-hours (675 000 GB-s): the plan's "medium" month.
    const usd = paidUsd({ requests: 22_500_000, awakeSeconds: 1500 * 3600 }, PAID);
    expect(usd).toBeCloseTo(5 + 3.225 + 3.4375, 3);
  });

  it("turns near_limit at 80% of $10 and limit at $10", () => {
    const at = (usd: number) => status(usage({ requests: 1_000_000 + ((usd - 5) / 0.15) * 1e6, awakeSeconds: 0 }, ZERO, PAID), PAID);
    expect(at(7.9)).toBe("active");
    expect(at(8.1)).toBe("near_limit");
    expect(at(10.01)).toBe("limit");
  });

  it("counts the Free plan per day, against 90% of the quota", () => {
    const free = { ...PAID, plan: "free" as const };
    expect(status(usage(ZERO, { requests: 75_000, awakeSeconds: 0 }, free), free)).toBe("near_limit");
    expect(status(usage(ZERO, { requests: 91_000, awakeSeconds: 0 }, free), free)).toBe("limit");
    expect(status(usage(ZERO, { requests: 10_000, awakeSeconds: 13_000 * 8 * 0.5 }, free), free)).toBe("active");
  });

  it("RELAY_OPEN=0 is the limit at once", () => {
    expect(status(0, { ...PAID, open: false })).toBe("limit");
    expect(settings({ RELAY_OPEN: "0" }).open).toBe(false);
    expect(settings({}).budgetUsd).toBe(10);
  });
});

function token(): string {
  return crypto.randomUUID().replace(/-/g, "");
}

async function sha256(t: string): Promise<string> {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(t));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// A client socket in the test runtime gets binary messages as a Blob.
function next(ws: WebSocket): Promise<string | ArrayBuffer> {
  return new Promise((resolve) =>
    ws.addEventListener("message", (e) => {
      const d = e.data as string | ArrayBuffer | Blob;
      resolve(d instanceof Blob ? d.arrayBuffer() : d);
    }, { once: true }),
  );
}

describe("relay", () => {
  it("answers /healthz", async () => {
    const res = await exports.default.fetch("http://relay/healthz");
    const h = await res.json<Record<string, unknown>>();
    expect(h).toMatchObject({ ok: true, relay: 1, status: "active", open: true, pipes: 7, max_message: 65536 });
  });

  it("pipes bytes between a host and a joiner", async () => {
    const room = token(), secret = token(), key = token();
    const hostRes = await exports.default.fetch(`http://relay/v1/rooms/${room}/host`, {
      headers: { upgrade: "websocket", "x-host-secret": secret, "x-join-key-hash": await sha256(key) },
    });
    expect(hostRes.status).toBe(101);
    const host = hostRes.webSocket!;
    host.accept();
    const hostHello = next(host);
    expect(JSON.parse((await hostHello) as string)).toMatchObject({ t: "hello", v: 1 });

    const bad = await exports.default.fetch(`http://relay/v1/rooms/${room}/join`, { headers: { upgrade: "websocket", "x-join-key": token() } });
    expect(bad.status).toBe(403);

    const opened = next(host);
    const joinRes = await exports.default.fetch(`http://relay/v1/rooms/${room}/join`, { headers: { upgrade: "websocket", "x-join-key": key } });
    expect(joinRes.status).toBe(101);
    const joiner = joinRes.webSocket!;
    joiner.accept();
    expect([...new Uint8Array((await opened) as ArrayBuffer)]).toEqual([1, 1]);

    const up = next(host);
    joiner.send(new Uint8Array([7, 8, 9]));
    expect([...new Uint8Array((await up) as ArrayBuffer)]).toEqual([2, 1, 7, 8, 9]);

    // The host ends the pipe: the joiner's socket closes.
    const gone = new Promise<number>((resolve) => joiner.addEventListener("close", (e) => resolve(e.code), { once: true }));
    host.send(new Uint8Array([3, 1]));
    expect(await gone).toBe(1000);
    host.close();
  });

  // Server-side closes are checked here because `wrangler dev`'s local proxy
  // does not pass them on to an outside client (conformance.py there).
  it("replaces a host and closes its joiners", async () => {
    const room = token(), secret = token(), key = token();
    const connect = async (side: string, headers: Record<string, string>) => {
      const res = await exports.default.fetch(`http://relay/v1/rooms/${room}/${side}`, { headers: { upgrade: "websocket", ...headers } });
      expect(res.status).toBe(101);
      const ws = res.webSocket!;
      ws.accept();
      return ws;
    };
    const closeCode = (ws: WebSocket) => new Promise<number>((resolve) => ws.addEventListener("close", (e) => resolve(e.code), { once: true }));
    const hostHeaders = { "x-host-secret": secret, "x-join-key-hash": await sha256(key) };
    const host = await connect("host", hostHeaders);
    const joiner = await connect("join", { "x-join-key": key });
    const hostClosed = closeCode(host), joinerClosed = closeCode(joiner);
    const host2 = await connect("host", hostHeaders);
    expect(await hostClosed).toBe(4000);
    expect(await joinerClosed).toBe(1001);
    const joiner2 = await connect("join", { "x-join-key": key });
    const joiner2Closed = closeCode(joiner2);
    host2.close();
    expect(await joiner2Closed).toBe(1001);
  });
});
