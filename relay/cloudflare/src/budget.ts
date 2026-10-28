// One Budget object per relay: the rooms report what they used, and anyone
// asks for the status. Counters per calendar month and per day (UTC), kept in
// the object's storage; old periods are dropped as new ones start.

import { DurableObject } from "cloudflare:workers";
import { Status, Usage, settings, status, usage } from "./cost";
import type { Env } from "./index";

export interface BudgetView {
  status: Status;
  usage: number;
  month: Usage;
  day: Usage;
}

const ZERO: Usage = { requests: 0, awakeSeconds: 0 };

function periods(t = new Date()): { month: string; day: string } {
  const iso = t.toISOString();
  return { month: `m:${iso.slice(0, 7)}`, day: `d:${iso.slice(0, 10)}` };
}

export class Budget extends DurableObject<Env> {
  private async read(): Promise<{ month: Usage; day: Usage; keys: { month: string; day: string } }> {
    const keys = periods();
    const got = await this.ctx.storage.get<Usage>([keys.month, keys.day]);
    return { month: got.get(keys.month) ?? ZERO, day: got.get(keys.day) ?? ZERO, keys };
  }

  async report(used: Usage): Promise<BudgetView> {
    const { month, day, keys } = await this.read();
    const add = (u: Usage): Usage => ({ requests: u.requests + used.requests, awakeSeconds: u.awakeSeconds + used.awakeSeconds });
    const next = { month: add(month), day: add(day) };
    const all = await this.ctx.storage.list({ prefix: "" });
    const stale = [...all.keys()].filter((k) => k !== keys.month && k !== keys.day);
    if (stale.length) await this.ctx.storage.delete(stale);
    await this.ctx.storage.put({ [keys.month]: next.month, [keys.day]: next.day });
    return this.view(next.month, next.day);
  }

  async check(): Promise<BudgetView> {
    const { month, day } = await this.read();
    return this.view(month, day);
  }

  private view(month: Usage, day: Usage): BudgetView {
    const s = settings(this.env as unknown as Record<string, unknown>);
    const u = usage(month, day, s);
    return { status: status(u, s), usage: Math.round(u * 1000) / 1000, month, day };
  }
}
