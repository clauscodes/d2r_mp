import { cloudflareTest } from "@cloudflare/vitest-plugin";
import { defineConfig } from "vitest/config";
import { GAME_PORT } from "./test/game_port";

// test/game_server.ts listens on GAME_PORT: a stand-in for a host's D2R that
// the bridge test reaches on loopback.

export default defineConfig({
  plugins: [
    cloudflareTest({
      wrangler: { configPath: "./wrangler.toml" },
      miniflare: { bindings: { BRIDGE_PORTS: `4000,${GAME_PORT}`, BRIDGE_ALLOW_PRIVATE: "1" } },
    }),
  ],
  test: { globalSetup: ["./test/game_server.ts"] },
});
