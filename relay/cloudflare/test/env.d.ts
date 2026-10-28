declare global {
  namespace Cloudflare {
    // Types `exports` from "cloudflare:workers": the Worker under test.
    interface GlobalProps {
      mainModule: typeof import("../src/index");
    }
  }
}

export {};
