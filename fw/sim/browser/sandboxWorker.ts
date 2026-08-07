/**
 * Web Worker entry for the extension sandbox — the browser twin of
 * node/sandboxWorker.ts. Deliberately thin: every message is a
 * SandboxRequest handed to SandboxCore, every reply a SandboxResponse.
 *
 * Running the extension here (rather than on the UI thread) is what makes
 * hang recovery possible at all: a spinning rgbx_tick never replies, the
 * host times out at the wall backstop and terminate()s this worker — the
 * simulator's stand-in for k_thread_abort on the device's sandbox thread.
 */

import { SandboxCore } from "../core/sandboxCore";
import type { SandboxRequest } from "../core/workerProtocol";

const ctx = self as unknown as DedicatedWorkerGlobalScope;
const core = new SandboxCore();

ctx.onmessage = (ev: MessageEvent<SandboxRequest>) => {
  void core.handle(ev.data).then((resp) => {
    ctx.postMessage(resp);
  });
};
