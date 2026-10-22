import type { D1Migration } from "cloudflare:test";
import type { Env } from "../src/board";

declare global {
  namespace Cloudflare {
    interface Env {
      DB: D1Database;
      TEST_MIGRATIONS: D1Migration[];
    }
    // Types `exports` from "cloudflare:workers": the Worker under test.
    interface GlobalProps {
      mainModule: typeof import("../src/index");
    }
  }
}

export type { Env };
