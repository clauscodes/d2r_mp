// What the relay costs on Cloudflare, from its own counters, and the status
// that follows (PROTOCOL.md §1.1). Prices are Cloudflare's published Durable
// Objects prices (2026-10); the relay cannot read the real bill, so this is an
// estimate that errs high (every message is counted, idle time is not).

export interface Usage {
  /** Incoming WebSocket messages (billed 20:1) plus connections, alarms and calls. */
  requests: number;
  /** Seconds a room object was awake with pipes open (billed at 128 MB). */
  awakeSeconds: number;
}

export interface Settings {
  plan: "paid" | "free";
  /** Paid: the whole monthly bill allowed, the plan's $5 included. */
  budgetUsd: number;
  /** Paid: the plan's monthly minimum. */
  planBaseUsd: number;
  /** Share of the cap that stops new rooms. */
  nearLimit: number;
  /** Free: share of the daily quota treated as the cap (past the quota Cloudflare cuts games). */
  freeShare: number;
  /** false = status "limit" whatever the counters say (maintenance). */
  open: boolean;
}

export type Status = "active" | "near_limit" | "limit";

export const GB_PER_OBJECT = 0.125;

export const PAID = {
  requestsIncluded: 1_000_000,
  usdPerMillionRequests: 0.15,
  gbsIncluded: 400_000,
  usdPerMillionGbs: 12.5,
};

export const FREE = {
  requestsPerDay: 100_000,
  gbsPerDay: 13_000,
};

/** Incoming WebSocket messages count 20:1 as requests. */
export function messagesToRequests(messages: number): number {
  return messages / 20;
}

export function paidUsd(month: Usage, s: Settings): number {
  const gbs = month.awakeSeconds * GB_PER_OBJECT;
  return (
    s.planBaseUsd +
    (Math.max(0, month.requests - PAID.requestsIncluded) * PAID.usdPerMillionRequests) / 1e6 +
    (Math.max(0, gbs - PAID.gbsIncluded) * PAID.usdPerMillionGbs) / 1e6
  );
}

/** 0..1+: how much of the cap is used. */
export function usage(month: Usage, day: Usage, s: Settings): number {
  if (s.plan === "free") {
    const share = Math.max(day.requests / FREE.requestsPerDay, (day.awakeSeconds * GB_PER_OBJECT) / FREE.gbsPerDay);
    return share / s.freeShare;
  }
  return paidUsd(month, s) / s.budgetUsd;
}

export function status(u: number, s: Settings): Status {
  if (!s.open || u >= 1) return "limit";
  return u >= s.nearLimit ? "near_limit" : "active";
}

function num(raw: string | undefined, def: number): number {
  const n = raw === undefined ? NaN : Number(raw);
  return Number.isFinite(n) && n > 0 ? n : def;
}

export function settings(env: Record<string, unknown>): Settings {
  const str = (k: string) => (typeof env[k] === "string" ? (env[k] as string) : undefined);
  return {
    plan: str("PLAN") === "free" ? "free" : "paid",
    budgetUsd: num(str("BUDGET_USD"), 10),
    planBaseUsd: num(str("PLAN_BASE_USD"), 5),
    nearLimit: num(str("NEAR_LIMIT"), 0.8),
    freeShare: num(str("FREE_SHARE"), 0.9),
    open: str("RELAY_OPEN") !== "0",
  };
}
