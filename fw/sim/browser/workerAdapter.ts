/**
 * SandboxAdapter over a Web Worker — same semantics as node/workerAdapter.ts:
 * request() rejects with SandboxTimeoutError when no reply arrives in time,
 * and terminate() kills the worker even mid-wasm-call (Worker.terminate is
 * the browser's equivalent of worker_threads' terminate, and is the only way
 * to recover from an extension that spins forever).
 */

import { SandboxAdapter, SandboxTimeoutError } from "../core/host";
import type { SandboxRequest, SandboxResponse } from "../core/workerProtocol";

interface Pending {
  resolve: (r: SandboxResponse) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

export class BrowserWorkerAdapter implements SandboxAdapter {
  private readonly worker: Worker;
  private readonly pending = new Map<number, Pending>();
  private dead = false;

  constructor() {
    this.worker = new Worker(new URL("./sandboxWorker.ts", import.meta.url), {
      type: "module",
    });
    this.worker.onmessage = (ev: MessageEvent<SandboxResponse>) => {
      const entry = this.pending.get(ev.data.id);
      if (entry !== undefined) {
        this.pending.delete(ev.data.id);
        clearTimeout(entry.timer);
        entry.resolve(ev.data);
      }
    };
    this.worker.onerror = (ev: ErrorEvent) => {
      this.dead = true;
      this.rejectAll(new Error(`sandbox worker died: ${ev.message}`));
    };
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
      this.pending.set(req.id, { resolve, reject, timer });
      this.worker.postMessage(req);
    });
  }

  async terminate(): Promise<void> {
    this.dead = true;
    this.rejectAll(new Error("sandbox terminated"));
    this.worker.terminate();
  }

  private rejectAll(err: Error): void {
    for (const [, entry] of this.pending) {
      clearTimeout(entry.timer);
      entry.reject(err);
    }
    this.pending.clear();
  }
}
