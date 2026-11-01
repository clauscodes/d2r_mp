// The cost model and one round trip inside the Workers runtime. The full
// protocol is checked by ../conformance.py against `wrangler dev`.

import { exports } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import { bridgePorts, bridgeTarget } from "../src/bridge";
import { Settings, paidUsd, settings, status, usage } from "../src/cost";
import { GAME_PORT } from "./game_port";

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

describe("bridge", () => {
  it("takes only public addresses and the game's ports", () => {
    const ports = bridgePorts("4000");
    expect(bridgeTarget("188.234.245.4", "4000", ports)).toEqual({ hostname: "188.234.245.4", port: 4000 });
    expect(bridgeTarget("2a01:4f8:c17:1::1", "4000", ports)).toEqual({ hostname: "2a01:4f8:c17:1::1", port: 4000 });
    expect(bridgeTarget("188.234.245.4", "22", ports)).toBe("bad_port");
    expect(bridgeTarget("188.234.245.4", "40000", ports)).toBe("bad_port");
    for (const a of ["127.0.0.1", "10.8.0.1", "192.168.1.5", "172.20.0.1", "100.64.3.4", "169.254.1.1", "0.0.0.0", "224.0.0.1",
      "::1", "::", "fe80::1", "fd00::5", "::ffff:8.8.8.8", "256.1.1.1", "1.2.3", "01.2.3.4", "example.com", "1:2:3:4:5:6:7:8:9"])
      expect(bridgeTarget(a, "4000", ports), a).toBe("bad_address");
    expect(bridgeTarget("127.0.0.1", "4000", ports, true)).toEqual({ hostname: "127.0.0.1", port: 4000 });
    expect(bridgePorts(" 4000, 4001,x,70000")).toEqual([4000, 4001]);
    expect(bridgePorts("")).toEqual([4000]);
  });

  it("says so in /healthz", async () => {
    const h = await (await exports.default.fetch("http://relay/healthz")).json<Record<string, unknown>>();
    expect(h).toMatchObject({ bridge: true, bridge_ports: [4000, GAME_PORT] });
  });

  it("refuses what it may not reach", async () => {
    const ask = (path: string, upgrade = true) =>
      exports.default.fetch(`http://relay/v1/bridge/${path}`, { headers: upgrade ? { upgrade: "websocket" } : {} });
    expect((await ask("188.234.245.4/22")).status).toBe(400);
    expect((await ask(`127.0.0.1/${GAME_PORT}`, false)).status).toBe(426);
    // Nobody listens on 4000 here: the host cannot be reached, said before the upgrade.
    const res = await ask("127.0.0.1/4000");
    expect(res.status).toBe(502);
    expect(await res.json()).toEqual({ error: "unreachable" });
  });

  it("pipes a joiner to the host's own port", async () => {
    const res = await exports.default.fetch(`http://relay/v1/bridge/127.0.0.1/${GAME_PORT}`, { headers: { upgrade: "websocket" } });
    expect(res.status).toBe(101);
    const ws = res.webSocket!;
    const got: (string | Uint8Array)[] = [];
    let wake: () => void = () => {};
    ws.addEventListener("message", async (e) => {
      const d = e.data as string | ArrayBuffer | Blob;
      got.push(typeof d === "string" ? d : new Uint8Array(d instanceof Blob ? await d.arrayBuffer() : d));
      wake();
    });
    ws.accept();
    const until = async (ok: () => boolean) => {
      while (!ok()) await new Promise<void>((r) => { wake = r; setTimeout(r, 200); });
    };
    const bytes = () => new TextDecoder().decode(Uint8Array.from(got.filter((m) => typeof m !== "string").flatMap((m) => [...(m as Uint8Array)])));
    await until(() => got.length > 0);
    expect(JSON.parse(got[0] as string)).toMatchObject({ t: "hello", v: 1, bridge: true });
    // The host speaks first, then echoes.
    await until(() => bytes() === "hi");
    ws.send(new TextEncoder().encode("join"));
    await until(() => bytes() === "hijoin");
    ws.send('{"t":"ping"}');
    await until(() => got.includes('{"t":"pong"}'));
    ws.close(1000, "done");
  });
});
