/**
 * SimHost — the tick orchestrator. Mirrors extension_host::tick()
 * (fw/src/extensions/extension_host.cpp:1125-1252) plus the activate /
 * fault / param lifecycle around it:
 *
 *  - dt_ms is a constant NOMINAL value (default 11 — ~90 Hz), never
 *    measured time; the sim clock advances by exactly dtMs per tick.
 *  - paramValues stay RAW (mode-carrying for COLOR params) — authoritative
 *    for reads/UI — while the value written into rgbx_inputs.params[p] is
 *    the color-mode-RESOLVED 0x00RRGGBB.
 *  - audio features refresh on 32 ms boundaries and hold between (sticky
 *    beats); IMU refreshes on 40 ms boundaries and holds between.
 *  - buttons are edge-latched: pressed-since-previous-tick bitmask,
 *    cleared after delivery.
 *  - tick-time faults (trap / cpu budget / wall backstop) reset params to
 *    manifest defaults; load/init failures do NOT (extension_host.cpp
 *    sandbox_fault resetParams semantics). A faulted host refuses tick()
 *    until clearFault() + activate() reload it fresh.
 */

import {
  INPUTS,
  NUM_BUTTONS,
  RGBX_MAX_PARAMS,
  RGBX_MAX_STRING_PARAMS,
  RGBX_PARAM_STRING_MAX,
  RgbxParamType,
  TickInputs,
  makeZeroInputs,
  setStringSlot,
  writeInputs,
} from "./abi";
import { ColorModeResolver, sweepPhaseOffset } from "./colorMode";
import { mulberry32 } from "./rng";
import type { ManifestMetadata } from "./manifest";
import type { ManifestResult } from "./manifest";
import {
  AUDIO_FRAME_MS,
  AudioFeatureProvider,
  AudioFeatures,
  BeatCounter,
  BeatCursor,
  IMU_PERIOD_MS,
  ImuProvider,
  SilenceAudioProvider,
  StaticImuProvider,
  zeroAudioFeatures,
} from "./providers";
import type { SandboxRequest, SandboxResponse } from "./workerProtocol";

/** Platform adapter around the sandbox worker. request() must reject with
 * SandboxTimeoutError when no response arrives within timeoutMs, and the
 * adapter must be terminate()d afterwards (the worker may be spinning). */
export interface SandboxAdapter {
  request(req: SandboxRequest, timeoutMs: number): Promise<SandboxResponse>;
  terminate(): Promise<void>;
}

export class SandboxTimeoutError extends Error {}

export type FaultKind =
  | "trap"
  | "cpu_budget"
  | "wall_backstop"
  | "init_failed"
  | "bad_manifest"
  | "load_failed";

export interface FaultInfo {
  kind: FaultKind;
  detail: string;
  /** Sim tick index at which the fault occurred (-1 for load/init). */
  tick: number;
  paramsResetToDefaults: boolean;
  manifestResult?: ManifestResult;
}

export interface TickOk {
  status: "ok";
  /** Raw extension framebuffer (pre-brightness, pre-dead-mask). */
  framebuffer: Uint8Array;
  goodMoment: boolean;
  log: string;
  wallMs: number;
  manifestIntact: boolean;
  /** Bitmask of audio bands whose beat flag was delivered this tick
   * (sticky across the ~3 ticks an audio frame covers, like the device). */
  beatMask: number;
}

export interface TickFault {
  status: "fault";
  fault: FaultInfo;
  log: string;
}

export type TickOutcome = TickOk | TickFault;

export interface SimHostOptions {
  wasmBytes: ArrayBuffer;
  adapterFactory: () => SandboxAdapter;
  /** Nominal per-tick dt — kTargetRenderIntervalMs truncated (default 11). */
  dtMs?: number;
  /** CONFIG_APP_EXT_TICK_CPU_BUDGET_MS analog, checked against the tick's
   * in-worker wall time (see PARITY.md — a coarse stand-in). Default 50. */
  budgetMs?: number;
  /** CONFIG_APP_EXT_TICK_WALL_BACKSTOP_MS analog: real-time reply timeout,
   * after which the worker is terminated. Default 500. */
  backstopMs?: number;
  seed?: number;
  expectedWidth?: number;
  expectedHeight?: number;
  audioProvider?: AudioFeatureProvider;
  imuProvider?: ImuProvider;
}

export class SimHost {
  readonly dtMs: number;
  readonly budgetMs: number;
  readonly backstopMs: number;

  private readonly wasmBytes: ArrayBuffer;
  private readonly adapterFactory: () => SandboxAdapter;
  private readonly expectedWidth: number;
  private readonly expectedHeight: number;
  private readonly rng: () => number;

  audioProvider: AudioFeatureProvider;
  imuProvider: ImuProvider;
  readonly beatCounter = new BeatCounter();

  private adapter: SandboxAdapter | null = null;
  private requestId = 0;

  metadata: ManifestMetadata | null = null;
  private hasGoodMoment = false;

  /** Host-authoritative values — RAW mode-carrying words for COLOR params
   * (what BLE reads / the UI shows), like Slot::paramValues on the device. */
  readonly paramValues = new Uint32Array(RGBX_MAX_PARAMS);
  readonly stringValues = new Uint8Array(RGBX_MAX_STRING_PARAMS * RGBX_PARAM_STRING_MAX);
  private colorResolvers: (ColorModeResolver | null)[] = [];

  faulted = false;
  faultInfo: FaultInfo | null = null;

  /** Sim clock: exactly tickIndex * dtMs (nominal, like the device). */
  simTimeMs = 0;
  tickIndex = 0;
  private audioFramesDelivered = 0;
  private lastAudioFeatures: AudioFeatures = zeroAudioFeatures();
  private lastImuSampleIndex = -1;
  private lastImu = { accel: [0, 0, 0] as [number, number, number], gyro: [0, 0, 0] as [number, number, number] };
  private pendingButtons = 0;

  constructor(opts: SimHostOptions) {
    this.wasmBytes = opts.wasmBytes;
    this.adapterFactory = opts.adapterFactory;
    this.dtMs = opts.dtMs ?? 11;
    this.budgetMs = opts.budgetMs ?? 50;
    this.backstopMs = opts.backstopMs ?? 500;
    this.expectedWidth = opts.expectedWidth ?? 40;
    this.expectedHeight = opts.expectedHeight ?? 12;
    this.rng = mulberry32(opts.seed ?? 0);
    this.audioProvider = opts.audioProvider ?? new SilenceAudioProvider();
    this.imuProvider = opts.imuProvider ?? new StaticImuProvider();
  }

  /** Loads + validates + runs rgbx_init — the activate()+lazy-load path.
   * Returns null on success or the fault info on failure. Params are
   * seeded from manifest defaults only on the FIRST activation (or after a
   * param-resetting fault), like scan_slot() + sandbox_fault. */
  async activate(): Promise<FaultInfo | null> {
    if (this.adapter !== null) {
      await this.adapter.terminate();
      this.adapter = null;
    }
    this.faulted = false;
    this.faultInfo = null;
    this.adapter = this.adapterFactory();

    const loadResp = await this.send(
      {
        id: ++this.requestId,
        type: "load",
        wasmBytes: this.wasmBytes,
        expectedWidth: this.expectedWidth,
        expectedHeight: this.expectedHeight,
      },
      this.backstopMs,
    );
    if (loadResp === "timeout" || !loadResp.ok) {
      // Terminate on EVERY load-stage failure, not just clean rejections: a
      // load timeout means _initialize (C++ static ctors) may be spinning,
      // and an unterminated worker keeps burning a core behind the fault
      // banner (see the adapter contract note above SandboxAdapter).
      await this.terminate();
      const detail = loadResp === "timeout" ? "load timed out" : loadResp.message;
      const kind: FaultKind =
        loadResp !== "timeout" && loadResp.kind === "bad_manifest" ? "bad_manifest" : "load_failed";
      return this.fail(kind, detail, false, loadResp !== "timeout" && loadResp.ok === false ? loadResp.manifestResult : undefined);
    }
    if (loadResp.type !== "load") {
      await this.terminate();
      return this.fail("load_failed", "protocol error", false);
    }

    const firstActivation = this.metadata === null;
    this.metadata = loadResp.metadata;
    this.hasGoodMoment = loadResp.hasGoodMoment;
    if (firstActivation) {
      this.resetParamsToDefaults();
    }

    // One resolver per COLOR param; arm resets exactly like
    // extension_host::activate() arms every resolver's notifyActivated().
    //
    // Two details mirror the firmware's issue #344 fix. Each resolver gets its OWN beat
    // cursor over the shared counter, so with two colours on RandomOnBeat both re-roll
    // rather than the first consuming the beat and the second never seeing one. And each
    // gets a distinct sweep phase keyed on its COLOR ORDINAL (not its raw param index),
    // so two concurrent Spectrum Sweeps are half a wheel apart instead of bit-identical,
    // while a sole COLOR param stays at offset 0 regardless of where it sits among the
    // other params.
    const colorParamCount = this.metadata.params.filter(
      (p) => p.type === RgbxParamType.Color,
    ).length;
    let colorOrdinal = 0;
    this.colorResolvers = this.metadata.params.map((p) => {
      if (p.type !== RgbxParamType.Color) {
        return null;
      }
      const cursor = new BeatCursor(this.beatCounter);
      const resolver = new ColorModeResolver(
        this.rng,
        () => this.simTimeMs,
        () => cursor.consume(),
        sweepPhaseOffset(colorOrdinal++, colorParamCount),
      );
      return resolver;
    });
    for (const r of this.colorResolvers) {
      r?.notifyActivated();
    }

    // rgbx_init shares the tick deadline semantics (one backstop covers
    // load + init on the device's lazy-load path).
    const initResp = await this.send({ id: ++this.requestId, type: "init" }, this.backstopMs);
    if (initResp === "timeout") {
      await this.terminate();
      return this.fail("wall_backstop", "rgbx_init exceeded the wall backstop", false);
    }
    if (!initResp.ok) {
      await this.terminate();
      return this.fail("init_failed", initResp.message, false);
    }
    return null;
  }

  /** Mirrors `ext select` clearing a fault so activate() may run again. */
  clearFault(): void {
    this.faulted = false;
    this.faultInfo = null;
  }

  async terminate(): Promise<void> {
    if (this.adapter !== null) {
      await this.adapter.terminate();
      this.adapter = null;
    }
  }

  /** Queue a button press; delivered as an edge on the NEXT tick only. */
  pressButton(id: number): void {
    if (id >= 0 && id < NUM_BUTTONS) {
      this.pendingButtons |= 1 << id;
    }
  }

  /** Sets a scalar param (raw word — COLOR values keep their mode byte). */
  setParam(index: number, value: number): void {
    if (index >= 0 && index < RGBX_MAX_PARAMS) {
      this.paramValues[index] = value >>> 0;
    }
  }

  /** Writes a string param by PARAM index (maps to its string slot). */
  setStringParam(index: number, value: string): void {
    const slot = this.metadata?.params[index]?.stringSlot;
    if (slot !== undefined && slot !== 0xff) {
      setStringSlot(this.stringValues, slot, value);
    }
  }

  paramIndexByName(name: string): number {
    return this.metadata?.params.findIndex((p) => p.name === name) ?? -1;
  }

  private resetParamsToDefaults(): void {
    this.paramValues.fill(0);
    this.stringValues.fill(0);
    const meta = this.metadata;
    if (meta === null) {
      return;
    }
    meta.params.forEach((p, i) => {
      this.paramValues[i] = p.defaultValue >>> 0;
    });
    meta.stringDefaults.forEach((def, slot) => {
      setStringSlot(this.stringValues, slot, def);
    });
  }

  /** Builds the rgbx_inputs snapshot for the CURRENT sim time — the exact
   * analog of the input-snapshot block in extension_host::tick(). */
  private async buildInputs(): Promise<TickInputs> {
    const inputs = makeZeroInputs(this.dtMs);

    // Audio: deliver every 32 ms frame boundary crossed so far; features
    // (incl. beat flags) hold between deliveries.
    while (this.audioFramesDelivered * AUDIO_FRAME_MS <= this.simTimeMs) {
      const frame = await this.audioProvider.nextFrame(this.audioFramesDelivered);
      this.lastAudioFeatures = frame;
      this.beatCounter.onAudioFrame(frame);
      this.audioFramesDelivered++;
    }
    inputs.audioBandEnergy.set(this.lastAudioFeatures.bandEnergy);
    inputs.audioBeat.set(this.lastAudioFeatures.beat);
    inputs.audioDisplayBucket.set(this.lastAudioFeatures.displayBucket);

    // IMU: 25 Hz sample-and-hold.
    const imuIndex = Math.floor(this.simTimeMs / IMU_PERIOD_MS);
    if (imuIndex !== this.lastImuSampleIndex) {
      const s = this.imuProvider.sampleAt(imuIndex * IMU_PERIOD_MS);
      this.lastImu = { accel: [...s.accel], gyro: [...s.gyro] };
      this.lastImuSampleIndex = imuIndex;
    }
    inputs.accel.set(this.lastImu.accel);
    inputs.gyro.set(this.lastImu.gyro);

    // Params: raw copy, then COLOR params overwritten with the resolved
    // effective color (extension_host.cpp:1172-1181).
    inputs.params.set(this.paramValues);
    inputs.paramStrings.set(this.stringValues);
    const meta = this.metadata;
    if (meta !== null) {
      for (let p = 0; p < meta.paramCount; p++) {
        const resolver = this.colorResolvers[p];
        if (resolver !== null && resolver !== undefined) {
          inputs.params[p] = resolver.resolve(this.paramValues[p]) >>> 0;
        }
      }
    }

    // Buttons: edge-latched since the previous tick.
    inputs.buttonsPressed = this.pendingButtons;
    this.pendingButtons = 0;

    return inputs;
  }

  async tick(): Promise<TickOutcome> {
    if (this.faulted || this.adapter === null || this.metadata === null) {
      return {
        status: "fault",
        fault: this.faultInfo ?? { kind: "load_failed", detail: "not activated", tick: this.tickIndex, paramsResetToDefaults: false },
        log: "",
      };
    }

    const inputs = await this.buildInputs();
    const block = new ArrayBuffer(INPUTS.size);
    writeInputs(block, 0, inputs);

    const resp = await this.send(
      { id: ++this.requestId, type: "tick", inputsBlock: block },
      this.backstopMs,
    );

    if (resp === "timeout") {
      // Spinning or blocked. On the device a spin busts the 50 ms CPU
      // budget first; the sim cannot interrupt a running wasm call, so
      // both cases land on the wall backstop (PARITY.md).
      await this.terminate();
      return this.tickFault("wall_backstop", `no reply within ${this.backstopMs} ms — terminated`, "");
    }
    if (!resp.ok) {
      await this.terminate();
      return this.tickFault("trap", resp.message, resp.log ?? "");
    }
    if (resp.type !== "tick") {
      await this.terminate();
      return this.tickFault("trap", "protocol error", "");
    }
    if (resp.wallMs > this.budgetMs) {
      // Completed but over the CPU budget — on the device
      // CpuBudgetExceeded wins even over completion.
      await this.terminate();
      return this.tickFault("cpu_budget", `tick took ${resp.wallMs.toFixed(1)} ms (budget ${this.budgetMs} ms)`, resp.log);
    }

    this.tickIndex++;
    this.simTimeMs = this.tickIndex * this.dtMs;

    let beatMask = 0;
    for (let b = 0; b < inputs.audioBeat.length; b++) {
      if (inputs.audioBeat[b] !== 0) {
        beatMask |= 1 << b;
      }
    }

    return {
      status: "ok",
      framebuffer: new Uint8Array(resp.framebuffer),
      // Absent symbol = every frame is a good moment (ABI contract).
      goodMoment: !this.hasGoodMoment || resp.goodMoment !== 0,
      log: resp.log,
      wallMs: resp.wallMs,
      manifestIntact: resp.manifestIntact,
      beatMask,
    };
  }

  /** EVERY tick-time fault resets params to defaults (see the NOTE in
   * extension_tick_budget.h — sparing any case leaves the slot unable to
   * self-recover from a poisoned param). */
  private tickFault(kind: FaultKind, detail: string, log: string): TickFault {
    this.resetParamsToDefaults();
    this.faulted = true;
    this.faultInfo = { kind, detail, tick: this.tickIndex, paramsResetToDefaults: true };
    return { status: "fault", fault: this.faultInfo, log };
  }

  private fail(
    kind: FaultKind,
    detail: string,
    paramsReset: boolean,
    manifestResult?: ManifestResult,
  ): FaultInfo {
    this.faulted = true;
    this.faultInfo = { kind, detail, tick: -1, paramsResetToDefaults: paramsReset, manifestResult };
    return this.faultInfo;
  }

  private async send(
    req: SandboxRequest,
    timeoutMs: number,
  ): Promise<SandboxResponse | "timeout"> {
    if (this.adapter === null) {
      throw new Error("no adapter");
    }
    try {
      return await this.adapter.request(req, timeoutMs);
    } catch (err) {
      if (err instanceof SandboxTimeoutError) {
        return "timeout";
      }
      throw err;
    }
  }
}
