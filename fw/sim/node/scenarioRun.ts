/**
 * Executes one scenario against one extension .wasm — the engine behind
 * `rgbx-sim run`. Builds providers from the scenario, drives SimHost tick
 * by tick, applies timeline events, evaluates `expect` checks, and returns
 * the report object + exit classification.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import { performance } from "node:perf_hooks";
import { RgbxParamType } from "../core/abi";
import { DEFAULT_BRIGHTNESS_FACTOR, toDisplayedFrame } from "../core/display";
import { FaultInfo, SimHost } from "../core/host";
import { KeyframeImuProvider, RampImuProvider, SineImuProvider } from "../core/imuGen";
import {
  AudioFeatureProvider,
  FeatureReplayProvider,
  ImuProvider,
  SilenceAudioProvider,
  StaticImuProvider,
} from "../core/providers";
import { DspAudioProvider } from "../core/audio";
import { metronomePcm, noisePcm, samplesPcm, silencePcm, sweepPcm } from "../core/pcmGen";
import { BUTTON_INDEX, Scenario, ScenarioAudio, ScenarioImu } from "../core/scenario";
import { NodeWorkerAdapter } from "./workerAdapter";
import { decodeWavTo16kMono } from "./wav";
import { dLineFramesToFeatures, parseDLines } from "./dline";
import { Check, RunStats, buildReport, frameToAnsi } from "./report";
import { frameToPng } from "./png";

export interface RunOptions {
  wasmPath: string;
  dspWasmPath: string;
  scenario: Scenario;
  scenarioDir: string; // for resolving relative file references
  seed: number;
  ticks: number;
  /** Ticks to run BEFORE recording starts, so a scenario can be replayed from a
   *  non-zero animation time. Cost that grows with a free-running accumulator only
   *  shows up minutes in, which is otherwise unreachable outside a hardware session.
   *  Recorded tick indices are numbered from 0 at the START of the recorded window,
   *  so two runs with different warm-ups stay directly comparable. That applies to
   *  EVERY tick index in the report, `result.fault.tick` included — a fault during
   *  the warm-up therefore reports a negative tick, meaning "before the window".
   *  Callers must reject a non-zero value for scenarios carrying finite stimulus
   *  (a timeline, or a wav/features/sweep audio or ramp/keyframes IMU source):
   *  warm-up CONSUMES those rather than replaying them, so the recorded window
   *  would see none of the input the scenario exists to apply. `cli.ts` enforces
   *  this; `golden.ts` pins it to 0. */
  warmupTicks: number;
  dtMs: number;
  budgetMs: number;
  backstopMs: number;
  paramOverrides: Record<string, number | string>;
  outDir: string | null;
  asciiSamples: number;
  /** Exact ticks to sample (digest+ascii) — overrides asciiSamples spread.
   * Used by the golden comparator. */
  sampleTicksOverride?: number[];
  pngEvery: number; // 0 = off
  dumpFrames: number; // stride, 0 = off
  ansiEvery: number; // live terminal preview stride, 0 = off
  postBrightness: boolean; // render artifacts through the device brightness
}

export interface RunResult {
  report: Record<string, unknown>;
  /** 0 ok, 2 unexpected fault, 3 expectation failure. */
  exitCode: 0 | 2 | 3;
}

async function buildAudioProvider(
  audio: ScenarioAudio | undefined,
  opts: RunOptions,
): Promise<AudioFeatureProvider> {
  if (audio === undefined || audio.type === "silence") {
    return new SilenceAudioProvider();
  }
  if (audio.type === "features") {
    const text = fs.readFileSync(path.resolve(opts.scenarioDir, audio.file), "utf8");
    return new FeatureReplayProvider(dLineFramesToFeatures(parseDLines(text)));
  }
  // All remaining types synthesize PCM and need the real DSP module.
  if (!fs.existsSync(opts.dspWasmPath)) {
    throw new Error(
      `${opts.dspWasmPath} not built (run fw/sim/build-extensions.sh) — required for audio type "${audio.type}"`,
    );
  }
  const buf = fs.readFileSync(opts.dspWasmPath);
  const dspBytes = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
  switch (audio.type) {
    case "metronome":
      return DspAudioProvider.create(dspBytes, metronomePcm(audio));
    case "sweep":
      return DspAudioProvider.create(
        dspBytes,
        sweepPcm({ ...audio, durationMs: audio.durationMs ?? opts.scenario.durationMs }),
      );
    case "noise":
      return DspAudioProvider.create(
        dspBytes,
        noisePcm({ ...audio, seed: audio.seed ?? opts.seed }),
      );
    case "wav": {
      const samples = decodeWavTo16kMono(
        fs.readFileSync(path.resolve(opts.scenarioDir, audio.file)),
      );
      return DspAudioProvider.create(dspBytes, samplesPcm(samples));
    }
  }
}

function buildImuProvider(imu: ScenarioImu | undefined, durationMs: number): ImuProvider {
  if (imu === undefined) {
    return new StaticImuProvider();
  }
  switch (imu.type) {
    case "static":
      return new StaticImuProvider({
        accel: imu.accel ?? [0, 0, 9.81],
        gyro: imu.gyro ?? [0, 0, 0],
      });
    case "ramp":
      return new RampImuProvider({
        fromAccel: imu.fromAccel,
        toAccel: imu.toAccel,
        fromGyro: imu.fromGyro,
        toGyro: imu.toGyro,
        startMs: imu.startMs ?? 0,
        endMs: imu.endMs ?? durationMs,
      });
    case "sine":
      return new SineImuProvider({
        channel: imu.channel ?? "accel",
        axis: imu.axis,
        amplitude: imu.amplitude,
        hz: imu.hz,
      });
    case "keyframes":
      return new KeyframeImuProvider(imu.frames);
  }
}

/** Parses a scalar param value: decimal, 0x hex, or true/false. Returns
 * null when the token is not scalar-shaped. NEVER applied to STRING params
 * — their values pass through verbatim (a STRING param legitimately holds
 * the literal text "true" or "0x10"). */
export function parseScalarParamValue(raw: number | string): number | null {
  if (typeof raw === "number") {
    return raw;
  }
  if (/^(0x[0-9a-fA-F]+|\d+)$/.test(raw)) {
    return Number(raw);
  }
  if (raw === "true") {
    return 1;
  }
  if (raw === "false") {
    return 0;
  }
  return null;
}

function applyParam(host: SimHost, name: string, rawValue: number | string): string | null {
  const idx = host.paramIndexByName(name);
  if (idx < 0) {
    return `no param named "${name}" (have: ${host.metadata?.params.map((p) => p.name).join(", ")})`;
  }
  // Type first, coercion second: STRING params take the raw token verbatim.
  const type = host.metadata!.params[idx].type;
  if (type === RgbxParamType.String) {
    host.setStringParam(idx, String(rawValue));
    return null;
  }
  const value = parseScalarParamValue(rawValue);
  if (value === null) {
    return `param "${name}" expects a number (decimal, 0x hex, or true/false), got "${rawValue}"`;
  }
  host.setParam(idx, value);
  return null;
}

export async function runScenario(opts: RunOptions): Promise<RunResult> {
  const wallStart = performance.now();
  const scenario = opts.scenario;

  const buf = fs.readFileSync(opts.wasmPath);
  const wasmBytes = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);

  const audioProvider = await buildAudioProvider(scenario.audio, opts);
  const imuProvider = buildImuProvider(scenario.imu, scenario.durationMs);

  const host = new SimHost({
    wasmBytes,
    adapterFactory: () => new NodeWorkerAdapter(),
    dtMs: opts.dtMs,
    budgetMs: opts.budgetMs,
    backstopMs: opts.backstopMs,
    seed: opts.seed,
    audioProvider,
    imuProvider,
  });

  // Evenly spread ASCII sample ticks across the run (always include an
  // early tick so "renders immediately" is visible).
  const sampleTicks = new Set<number>(opts.sampleTicksOverride ?? []);
  if (sampleTicks.size === 0) {
    // First sample lands EARLY (~tick 10, ~110 ms) so a startup-only
    // rendering bug is visible in the report; the rest spread to the end.
    const n = Math.max(1, opts.asciiSamples);
    const early = Math.min(10, opts.ticks - 1);
    if (n === 1) {
      sampleTicks.add(opts.ticks - 1);
    } else {
      for (let i = 0; i < n; i++) {
        sampleTicks.add(Math.round(early + (i / (n - 1)) * (opts.ticks - 1 - early)));
      }
    }
  }
  const stats = new RunStats(sampleTicks);
  const checks: Check[] = [];
  const artifacts: Record<string, string | null> = { report: null, framesDir: null };

  let fault: FaultInfo | null = await host.activate();
  const timeline = [...(scenario.timeline ?? [])].sort((a, b) => a.atMs - b.atMs);
  let timelineAt = 0;

  if (fault === null) {
    // Apply CLI overrides at t=0 (after defaults, before the first tick).
    for (const [name, value] of Object.entries(opts.paramOverrides)) {
      const err = applyParam(host, name, value);
      if (err !== null) {
        throw new Error(err);
      }
    }

    // Timeline events due at or before the CURRENT sim time fire before the tick
    // that first covers them. Shared by the warm-up and recording loops so a
    // warmed-up run sees exactly the same event ordering as a cold one.
    const pumpTimeline = (): void => {
      while (timelineAt < timeline.length && timeline[timelineAt].atMs <= host.simTimeMs) {
        const ev = timeline[timelineAt++];
        if (ev.set !== undefined) {
          for (const [name, value] of Object.entries(ev.set)) {
            const err = applyParam(host, name, value);
            if (err !== null) {
              throw new Error(`timeline @${ev.atMs}ms: ${err}`);
            }
          }
        }
        if (ev.press !== undefined) {
          host.pressButton(BUTTON_INDEX[ev.press]);
        }
      }
    };

    // Every tick index that leaves this function is relative to the RECORDED
    // window, so one report never mixes two origins. host.tickIndex counts warm-up
    // ticks too, and fault.tick is stamped from it (core/host.ts) — left absolute, a
    // fault on the 5th recorded tick of a 5455-tick warm-up reports "tick 5459"
    // while frames.samples[].tick, firstNonBlackTick and the [tick N] log prefixes
    // all stop at opts.ticks - 1, so the fault cannot be located in the run it is
    // reported against. Warm-up faults go negative, which is the honest answer:
    // it happened before the window (-1 keeps its existing "load/init" meaning).
    const toRecordedTick = (absolute: number): number =>
      absolute < 0 ? absolute : absolute - opts.warmupTicks;

    // Warm-up ticks advance the extension's internal state but are NOT recorded:
    // no frames, no timing samples, no checks. A fault here is still a fault and
    // stops the run.
    for (let t = 0; t < opts.warmupTicks && fault === null; t++) {
      pumpTimeline();
      const warm = await host.tick();
      if (warm.status === "fault") {
        fault = { ...warm.fault, tick: toRecordedTick(warm.fault.tick) };
      }
    }

    for (let t = 0; fault === null && t < opts.ticks; t++) {
      pumpTimeline();

      const out = await host.tick();
      if (out.status === "fault") {
        fault = { ...out.fault, tick: toRecordedTick(out.fault.tick) };
        break;
      }
      stats.record(
        host.tickIndex - 1 - opts.warmupTicks,
        out.framebuffer,
        out.wallMs,
        out.beatMask,
        out.goodMoment,
        out.manifestIntact,
        out.log,
      );

      if (opts.ansiEvery > 0 && (host.tickIndex - 1) % opts.ansiEvery === 0) {
        const frame = opts.postBrightness
          ? toDisplayedFrame(out.framebuffer, DEFAULT_BRIGHTNESS_FACTOR)
          : out.framebuffer;
        process.stdout.write(`\x1b[H\x1b[2Jtick ${host.tickIndex - 1}\n${frameToAnsi(frame)}\n`);
      }
      if (opts.outDir !== null) {
        const renderFrame = () =>
          opts.postBrightness
            ? toDisplayedFrame(out.framebuffer, DEFAULT_BRIGHTNESS_FACTOR)
            : out.framebuffer;
        if (opts.pngEvery > 0 && (host.tickIndex - 1) % opts.pngEvery === 0) {
          const dir = path.join(opts.outDir, "frames");
          fs.mkdirSync(dir, { recursive: true });
          fs.writeFileSync(
            path.join(dir, `tick-${String(host.tickIndex - 1).padStart(5, "0")}.png`),
            frameToPng(renderFrame()),
          );
          artifacts.framesDir = dir;
        }
        if (opts.dumpFrames > 0 && (host.tickIndex - 1) % opts.dumpFrames === 0) {
          const dir = path.join(opts.outDir, "raw");
          fs.mkdirSync(dir, { recursive: true });
          fs.writeFileSync(
            path.join(dir, `tick-${String(host.tickIndex - 1).padStart(5, "0")}.rgb`),
            out.framebuffer,
          );
        }
      }
    }
  }
  await host.terminate();

  // ---- expectations ------------------------------------------------------
  const expect = scenario.expect ?? {};
  const expectedFault = expect.fault?.kind ?? null;

  if (expectedFault !== null) {
    const pass = fault !== null && fault.kind === expectedFault;
    checks.push({
      name: `fault.kind==${expectedFault}`,
      pass,
      detail: fault === null ? "ran to completion without the expected fault" : `got ${fault.kind}`,
    });
  } else {
    checks.push({
      name: "no_fault",
      pass: fault === null,
      detail: fault === null ? undefined : `${fault.kind}: ${fault.detail}`,
    });
  }

  // `stats.ticks > 0` is not redundant with the fault check. A fault during the
  // WARM-UP stops the run before a single frame is recorded, and the condition above
  // stays true whenever the scenario expected a fault — so without this, a crash
  // scenario that also sets nonBlackBeforeMs/visibleAfterBrightness exits 3
  // "expectation failed" (firstNonBlackTick=-1, visibleAfterBrightnessTicks=0)
  // despite getting exactly the fault it asked for. These checks describe the
  // RECORDED window; with no recorded window there is nothing for them to describe.
  if ((fault === null || expectedFault !== null) && stats.ticks > 0) {
    if (expect.nonBlackBeforeMs !== undefined) {
      const tickBound = Math.ceil(expect.nonBlackBeforeMs / opts.dtMs);
      const pass = stats.firstNonBlackTick >= 0 && stats.firstNonBlackTick <= tickBound;
      checks.push({
        name: `non_black_before_${expect.nonBlackBeforeMs}ms`,
        pass,
        detail: `firstNonBlackTick=${stats.firstNonBlackTick}`,
      });
    }
    if (expect.visibleAfterBrightness === true) {
      checks.push({
        name: "visible_after_brightness",
        pass: stats.visibleAfterBrightnessTicks > 0,
        detail:
          stats.visibleAfterBrightnessTicks > 0
            ? undefined
            : "no pixel survives the device's x0.02 brightness truncation — render nearer full scale (255)",
      });
    }
    if (expect.beatResponse === true) {
      const br = stats.beatResponse();
      checks.push({
        name: "beat_response",
        pass: br.detected,
        detail: `onBeat=${br.onBeat.toFixed(2)} offBeat=${br.offBeat.toFixed(2)} ratio=${Number.isFinite(br.ratio) ? br.ratio.toFixed(2) : "inf"}`,
      });
    }
  }
  // Scribbling over const manifest data would MPU-fault on the device.
  if (stats.manifestViolationTicks > 0) {
    checks.push({
      name: "manifest_rodata_intact",
      pass: false,
      detail: `${stats.manifestViolationTicks} tick(s) modified const manifest data — this faults on hardware`,
    });
  }

  const report = buildReport({
    metadata: host.metadata,
    wasmPath: opts.wasmPath,
    scenario,
    seed: opts.seed,
    dtMs: opts.dtMs,
    budgetMs: opts.budgetMs,
    stats,
    fault,
    checks,
    hostWallMs: performance.now() - wallStart,
    artifacts,
  });

  const unexpectedFault = fault !== null && expectedFault === null;
  const checksFailed = checks.some((c) => !c.pass);
  const exitCode = unexpectedFault ? 2 : checksFailed ? 3 : 0;
  return { report, exitCode };
}
