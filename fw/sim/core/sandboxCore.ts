/**
 * The sandbox worker's internals — platform-agnostic. Both worker entries
 * (browser Web Worker, Node worker_threads) construct one SandboxCore and
 * forward messages to handle().
 *
 * This code runs INSIDE the worker next to the extension instance. It is
 * the simulator's counterpart of the device's sandbox_entry() +
 * resolve_exports() (extension_host.cpp): instantiate, run ctors
 * (_initialize == llext_bringup), resolve the rgbx exports, then service
 * init/tick requests against the instance.
 */

import { FRAME_BYTES } from "./display";
import { INPUTS, MANIFEST, PARAM_DESC } from "./abi";
import { ManifestResult, validateManifest } from "./manifest";
import type { SandboxRequest, SandboxResponse } from "./workerProtocol";

interface ResolvedExports {
  memory: WebAssembly.Memory;
  rgbxInit: () => void;
  rgbxTick: () => void;
  manifestAddr: number;
  inputsAddr: number;
  framebufferAddr: number;
  goodMomentAddr: number | null;
  logBufAddr: number | null;
  logLenAddr: number | null;
}

function globalValue(exp: WebAssembly.Exports, name: string): number | null {
  const g = exp[name];
  return g instanceof WebAssembly.Global ? (g.value as number) : null;
}

export class SandboxCore {
  private ex: ResolvedExports | null = null;
  private frameBytes = FRAME_BYTES;
  /** Snapshot of the manifest struct + param table taken at load; the
   * device keeps this data in MPU-protected rodata, so a change here means
   * the extension did something that would fault on hardware. */
  private rodataSnapshot: Uint8Array | null = null;
  private rodataAddr = 0;

  async handle(req: SandboxRequest): Promise<SandboxResponse> {
    switch (req.type) {
      case "load":
        return this.load(req.id, req.wasmBytes, req.expectedWidth, req.expectedHeight);
      case "init":
        return this.call(req.id, "init");
      case "tick":
        return this.tick(req.id, req.inputsBlock);
    }
  }

  private async load(
    id: number,
    wasmBytes: ArrayBuffer,
    expectedWidth: number,
    expectedHeight: number,
  ): Promise<SandboxResponse> {
    let instance: WebAssembly.Instance;
    try {
      // Zero-import contract: an extension needing any import would also
      // fail llext symbol resolution on the device.
      const result = await WebAssembly.instantiate(wasmBytes, {});
      instance = result.instance;
      // _initialize (reactor model) runs C++ static constructors — the
      // sim analog of llext_bringup() running init arrays in the sandbox.
      const initialize = instance.exports["_initialize"];
      if (typeof initialize === "function") {
        (initialize as () => void)();
      }
    } catch (err) {
      return { id, ok: false, kind: "instantiate_failed", message: String(err) };
    }

    const exp = instance.exports;
    const memory = exp["memory"];
    const manifestAddr = globalValue(exp, "rgbx_manifest");
    const inputsAddr = globalValue(exp, "rgbx_inputs");
    const framebufferAddr = globalValue(exp, "rgbx_framebuffer");
    const rgbxInit = exp["rgbx_init"];
    const rgbxTick = exp["rgbx_tick"];
    if (
      !(memory instanceof WebAssembly.Memory) ||
      manifestAddr === null ||
      inputsAddr === null ||
      framebufferAddr === null ||
      typeof rgbxInit !== "function" ||
      typeof rgbxTick !== "function"
    ) {
      return {
        id,
        ok: false,
        kind: "instantiate_failed",
        message: "missing required rgbx exports",
      };
    }

    const outcome = validateManifest(memory.buffer, manifestAddr, {
      expectedWidth,
      expectedHeight,
    });
    if (outcome.result !== ManifestResult.Ok) {
      return {
        id,
        ok: false,
        kind: "bad_manifest",
        message: `manifest rejected: ${outcome.result}`,
        manifestResult: outcome.result,
      };
    }

    this.ex = {
      memory,
      rgbxInit: rgbxInit as () => void,
      rgbxTick: rgbxTick as () => void,
      manifestAddr,
      inputsAddr,
      framebufferAddr,
      goodMomentAddr: globalValue(exp, "rgbx_good_moment"),
      logBufAddr: globalValue(exp, "rgbx_sim_log_buf"),
      logLenAddr: globalValue(exp, "rgbx_sim_log_len"),
    };
    this.frameBytes = outcome.metadata.width * outcome.metadata.height * 3;

    // Snapshot manifest struct + param table for the per-tick const check.
    const view = new DataView(memory.buffer);
    const paramCount = view.getUint32(manifestAddr + MANIFEST.paramCount, true);
    const paramsPtr = view.getUint32(manifestAddr + MANIFEST.params, true);
    this.rodataAddr = manifestAddr;
    const manifestBytes = new Uint8Array(memory.buffer, manifestAddr, MANIFEST.size);
    if (paramCount > 0) {
      const tableBytes = new Uint8Array(
        memory.buffer,
        paramsPtr,
        paramCount * PARAM_DESC.size,
      );
      const snap = new Uint8Array(MANIFEST.size + tableBytes.length);
      snap.set(manifestBytes, 0);
      snap.set(tableBytes, MANIFEST.size);
      this.rodataSnapshot = snap;
      this.rodataTableAddr = paramsPtr;
      this.rodataTableLen = tableBytes.length;
    } else {
      this.rodataSnapshot = manifestBytes.slice();
      this.rodataTableAddr = 0;
      this.rodataTableLen = 0;
    }

    return { id, ok: true, type: "load", metadata: outcome.metadata, hasGoodMoment: this.ex.goodMomentAddr !== null };
  }

  private rodataTableAddr = 0;
  private rodataTableLen = 0;

  private drainLog(): string {
    const ex = this.ex;
    if (ex === null || ex.logBufAddr === null || ex.logLenAddr === null) {
      return "";
    }
    const view = new DataView(ex.memory.buffer);
    const len = view.getUint32(ex.logLenAddr, true);
    if (len === 0) {
      return "";
    }
    const bytes = new Uint8Array(ex.memory.buffer, ex.logBufAddr, Math.min(len, 2048));
    const text = new TextDecoder().decode(bytes);
    view.setUint32(ex.logLenAddr, 0, true);
    return text;
  }

  private checkManifestIntact(): boolean {
    const ex = this.ex;
    if (ex === null || this.rodataSnapshot === null) {
      return true;
    }
    const mem = new Uint8Array(ex.memory.buffer);
    const snap = this.rodataSnapshot;
    for (let i = 0; i < MANIFEST.size; i++) {
      if (mem[this.rodataAddr + i] !== snap[i]) {
        return false;
      }
    }
    for (let i = 0; i < this.rodataTableLen; i++) {
      if (mem[this.rodataTableAddr + i] !== snap[MANIFEST.size + i]) {
        return false;
      }
    }
    return true;
  }

  private call(id: number, which: "init"): SandboxResponse {
    const ex = this.ex;
    if (ex === null) {
      return { id, ok: false, kind: "instantiate_failed", message: "not loaded" };
    }
    const start = performance.now();
    try {
      ex.rgbxInit();
    } catch (err) {
      return { id, ok: false, kind: "trap", message: String(err), log: this.drainLog() };
    }
    return { id, ok: true, type: which, log: this.drainLog(), wallMs: performance.now() - start };
  }

  private tick(id: number, inputsBlock: ArrayBuffer): SandboxResponse {
    const ex = this.ex;
    if (ex === null) {
      return { id, ok: false, kind: "instantiate_failed", message: "not loaded" };
    }
    // Input snapshot: the host pre-serialized the whole rgbx_inputs block.
    new Uint8Array(ex.memory.buffer).set(
      new Uint8Array(inputsBlock, 0, INPUTS.size),
      ex.inputsAddr,
    );

    const start = performance.now();
    try {
      ex.rgbxTick();
    } catch (err) {
      return { id, ok: false, kind: "trap", message: String(err), log: this.drainLog() };
    }
    const wallMs = performance.now() - start;

    // The sandbox is quiescent between tick return and the next request —
    // the one safe point to read per-tick outputs (same as the device).
    const framebuffer = new Uint8Array(ex.memory.buffer, ex.framebufferAddr, this.frameBytes)
      .slice().buffer;
    const goodMoment =
      ex.goodMomentAddr === null
        ? null
        : new Uint8Array(ex.memory.buffer)[ex.goodMomentAddr];

    return {
      id,
      ok: true,
      type: "tick",
      framebuffer,
      goodMoment,
      log: this.drainLog(),
      wallMs,
      manifestIntact: this.checkManifestIntact(),
    };
  }
}
