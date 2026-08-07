/**
 * SandboxAdapter over node:worker_threads. terminate() is the hang
 * recovery: worker_threads can kill a thread stuck inside a wasm call
 * (the sim analog of k_thread_abort on the device's sandbox thread).
 */

import { Worker } from "node:worker_threads";
import * as path from "node:path";
import { SandboxAdapter, SandboxTimeoutError } from "../core/host";
import type { SandboxRequest, SandboxResponse } from "../core/workerProtocol";

export class NodeWorkerAdapter implements SandboxAdapter {
  private worker: Worker;
  private pending = new Map<
    number,
    { resolve: (r: SandboxResponse) => void; reject: (e: Error) => void; timer: NodeJS.Timeout }
  >();
  private dead = false;

  constructor() {
    this.worker = new Worker(path.join(__dirname, "sandboxWorker.js"));
    this.worker.unref();
    this.worker.on("message", (resp: SandboxResponse) => {
      const entry = this.pending.get(resp.id);
      if (entry !== undefined) {
        this.pending.delete(resp.id);
        clearTimeout(entry.timer);
        entry.resolve(resp);
      }
    });
    this.worker.on("error", (err) => {
      this.dead = true;
      this.rejectAll(new Error(`sandbox worker died: ${err.message}`));
    });
  }

  request(req: SandboxRequest, timeoutMs: number): Promise<SandboxResponse> {
    if (this.dead) {
      return Promise.reject(new Error("sandbox worker is dead"));
    }
    return new Promise<SandboxResponse>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(req.id);
        reject(new SandboxTimeoutError(`no sandbox reply within ${timeoutMs} ms`));
      }, timeoutMs);
      // Don't let a pending backstop timer hold the process open.
      timer.unref();
      this.pending.set(req.id, { resolve, reject, timer });
      this.worker.postMessage(req);
    });
  }

  async terminate(): Promise<void> {
    this.dead = true;
    this.rejectAll(new Error("sandbox terminated"));
    await this.worker.terminate();
  }

  private rejectAll(err: Error): void {
    for (const [, entry] of this.pending) {
      clearTimeout(entry.timer);
      entry.reject(err);
    }
    this.pending.clear();
  }
}
