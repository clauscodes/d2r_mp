// A host's game for the bridge test, in Node: like D2R's host it speaks first
// ("hi"), then sends back every byte it gets.

import { createServer, Server } from "node:net";
import { GAME_PORT } from "./game_port";

let server: Server | null = null;

export async function setup(): Promise<void> {
  server = createServer((socket) => {
    socket.setNoDelay(true);
    socket.write("hi");
    socket.on("data", (d) => socket.write(d));
    socket.on("error", () => socket.destroy());
  });
  await new Promise<void>((resolve) => server!.listen(GAME_PORT, "127.0.0.1", resolve));
}

export async function teardown(): Promise<void> {
  await new Promise<void>((resolve) => (server ? server.close(() => resolve()) : resolve()));
}
