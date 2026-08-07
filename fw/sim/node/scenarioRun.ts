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

/** Parses a CLI/scenario param value: decimal, 0x hex, true/false, or a
 * plain string (only valid for STRING params). */
export function parseParamValue(raw: number | string): number | string {
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
  return raw;
}

function applyParam(host: SimHost, name: string, rawValue: number | string): string | null {
  const idx = host.paramIndexByName(name);
  if (idx < 0) {
    return `no param named "${name}" (have: ${host.metadata?.params.map((p) => p.name).join(", ")})`;
  }
  const value = parseParamValue(rawValue);
  const type = host.metadata!.params[idx].type;
  if (type === RgbxParamType.String) {
    host.setStringParam(idx, String(value));
  } else if (typeof value === "string") {
    return `param "${name}" is not a string param`;
  } else {
    host.setParam(idx, value);
  }
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
    const n = Math.max(1, opts.asciiSamples);
    for (let i = 0; i < n; i++) {
      sampleTicks.add(Math.max(1, Math.round(((i + 1) / n) * (opts.ticks - 1))));
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

    for (let t = 0; t < opts.ticks; t++) {
      // Timeline events due at or before the CURRENT sim time fire before
      // the tick that first covers them.
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

      const out = await host.tick();
      if (out.status === "fault") {
        fault = out.fault;
        break;
      }
      stats.record(
        host.tickIndex - 1,
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

  if (fault === null || expectedFault !== null) {
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
    metadata: host.metadata!,
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
