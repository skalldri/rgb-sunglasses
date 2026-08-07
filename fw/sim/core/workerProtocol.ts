/**
 * Message protocol between the SimHost and the sandbox worker.
 *
 * The extension executes on a worker (Web Worker in the browser,
 * worker_threads in Node) — the simulator's stand-in for the device's
 * K_USER sandbox thread. Running it off the host thread is what makes hang
 * recovery possible: a spinning rgbx_tick never replies, the host times out
 * at the wall backstop and terminates the worker, exactly as the device
 * tears down the sandbox thread.
 */

import type { ManifestMetadata, ManifestResult } from "./manifest";

export interface LoadRequest {
  id: number;
  type: "load";
  /** The extension .wasm bytes. */
  wasmBytes: ArrayBuffer;
  expectedWidth: number;
  expectedHeight: number;
}

export interface InitRequest {
  id: number;
  type: "init";
}

export interface TickRequest {
  id: number;
  type: "tick";
  /** Pre-serialized rgbx_inputs block (INPUTS.size bytes) — the worker
   * copies it verbatim into the extension's exported input struct. */
  inputsBlock: ArrayBuffer;
}

export type SandboxRequest = LoadRequest | InitRequest | TickRequest;

export interface LoadOkResponse {
  id: number;
  ok: true;
  type: "load";
  metadata: ManifestMetadata;
  /** Whether the optional rgbx_good_moment export is present. */
  hasGoodMoment: boolean;
}

export interface InitOkResponse {
  id: number;
  ok: true;
  type: "init";
  /** printk output drained after rgbx_init. */
  log: string;
  /** Wall time spent inside rgbx_init (ms). */
  wallMs: number;
}

export interface TickOkResponse {
  id: number;
  ok: true;
  type: "tick";
  /** Copy of rgbx_framebuffer after the tick (width*height*3 bytes). */
  framebuffer: ArrayBuffer;
  /** rgbx_good_moment value, or null when the export is absent. */
  goodMoment: number | null;
  /** printk output drained after the tick. */
  log: string;
  /** Wall time spent inside rgbx_tick (ms) — the sim's stand-in for the
   * device's per-tick CPU accounting (see PARITY.md). */
  wallMs: number;
  /** False if the extension scribbled over its own const manifest data —
   * on the device that write would MPU-fault (rodata is read-only); wasm
   * linear memory has no protection, so the worker diff-checks instead. */
  manifestIntact: boolean;
}

export interface ErrorResponse {
  id: number;
  ok: false;
  /** What failed:
   *  - instantiate_failed: WebAssembly.instantiate / bad module
   *  - bad_manifest: validation rejected (manifestResult set)
   *  - trap: rgbx_init/rgbx_tick trapped (wasm RuntimeError — the sim
   *    analog of an MPU fault) */
  kind: "instantiate_failed" | "bad_manifest" | "trap";
  message: string;
  manifestResult?: Exclude<ManifestResult, ManifestResult.Ok>;
  /** printk output drained before the failure (traps often follow logs). */
  log?: string;
}

export type SandboxResponse = LoadOkResponse | InitOkResponse | TickOkResponse | ErrorResponse;
