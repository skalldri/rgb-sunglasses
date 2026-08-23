/**
 * Executes one scenario against one extension .wasm — the engine behind
 * `rgbx-sim run`. Builds providers from the scenario, drives SimHost tick
 * by tick, applies timeline events, evaluates `expect` checks, and returns
 * the report object + exit classification.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import { performance } from "node:perf_hooks";
import { DEFAULT_BRIGHTNESS_FACTOR, toDisplayedFrame } from "../core/display";
import { FaultInfo, SimHost } from "../core/host";
import { Scenario } from "../core/scenario";
import { ScenarioIo, buildAudioProvider, buildImuProvider } from "../core/scenarioProviders";
import { TimelineRunner, applyParam } from "../core/scenarioTimeline";
import { NodeWorkerAdapter } from "./workerAdapter";
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

/** Node's ScenarioIo: `file:` refs resolve against the scenario's own
 * directory; DSP bytes come from the built out/wasm artifact. Provider
 * construction itself lives in core/scenarioProviders.ts, shared with the
 * browser scenario player. */
function makeScenarioIo(opts: RunOptions): ScenarioIo {
  return {
    readBytes: async (ref) => fs.readFileSync(path.resolve(opts.scenarioDir, ref)),
    readText: async (ref) => fs.readFileSync(path.resolve(opts.scenarioDir, ref), "utf8"),
    getDspBytes: async () => {
      if (!fs.existsSync(opts.dspWasmPath)) {
        throw new Error(`${opts.dspWasmPath} not built (run fw/sim/build-extensions.sh)`);
      }
      const buf = fs.readFileSync(opts.dspWasmPath);
      return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
    },
  };
}

export async function runScenario(opts: RunOptions): Promise<RunResult> {
  const wallStart = performance.now();
  const scenario = opts.scenario;

  const buf = fs.readFileSync(opts.wasmPath);
  const wasmBytes = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);

  const io = makeScenarioIo(opts);
  const audioProvider = await buildAudioProvider(scenario.audio, { scenario, seed: opts.seed, io });
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
  // Drain rgbx_init's printk before anything else can early-return: it is
  // populated on the fault path too, and an init fault is exactly when its
  // contents matter most.
  stats.recordInitLog(host.initLog);
  const timeline = new TimelineRunner(scenario.timeline);

  if (fault === null) {
    // Apply CLI overrides at t=0 (after defaults, before the first tick).
    for (const [name, value] of Object.entries(opts.paramOverrides)) {
      const err = applyParam(host, name, value);
      if (err !== null) {
        throw new Error(err);
      }
    }

    // Timeline events due at or before the CURRENT sim time fire before the tick
    // that first covers them (TimelineRunner, shared with the browser player).
    // Shared by the warm-up and recording loops so a warmed-up run sees exactly
    // the same event ordering as a cold one.
    const pumpTimeline = (): void => {
      timeline.pump(host);
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
        // Warm-up ticks are not recorded, but a fault during warm-up is just
        // as opaque without its log — and it reports a negative tick index,
        // which is exactly the signal that it happened before the window.
        stats.recordFaultLog(toRecordedTick(warm.fault.tick), warm.log);
        fault = { ...warm.fault, tick: toRecordedTick(warm.fault.tick) };
      }
    }

    for (let t = 0; fault === null && t < opts.ticks; t++) {
      pumpTimeline();

      const out = await host.tick();
      if (out.status === "fault") {
        // Drain BEFORE the break: TickFault carries the log the worker
        // captured on the way down, and it is the line that explains the
        // trap. The browser console already gets this right (stepOnce
        // appends outcome.log before handling the fault); only the report
        // dropped it.
        // Tag from the fault's own tick, NOT `host.tickIndex - 1 - warmup`
        // like the success path below: SimHost increments tickIndex only
        // after a tick survives, so on a fault it still points at the tick
        // that just died and the success-path arithmetic is one too low (a
        // trap on tick 0 came out as `[tick -1]`). Sharing the expression
        // with the `fault =` line means the log line and result.fault.tick
        // cannot disagree.
        stats.recordFaultLog(toRecordedTick(out.fault.tick), out.log);
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
