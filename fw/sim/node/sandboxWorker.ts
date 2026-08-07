/**
 * Node worker_threads entry for the extension sandbox. Wraps SandboxCore:
 * every parentPort message is a SandboxRequest; every reply a
 * SandboxResponse. A spinning extension keeps this thread busy forever —
 * the host terminates the whole worker at the wall backstop.
 */

import { parentPort } from "node:worker_threads";
import { SandboxCore } from "../core/sandboxCore";
import type { SandboxRequest } from "../core/workerProtocol";

if (parentPort === null) {
  throw new Error("sandboxWorker must run as a worker thread");
}
const port = parentPort;
const core = new SandboxCore();

port.on("message", (req: SandboxRequest) => {
  void core.handle(req).then((resp) => {
    port.postMessage(resp);
  });
});
