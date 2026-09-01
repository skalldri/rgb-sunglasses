/**
 * Run statistics + report.json (rgbx-sim-report/1) — the agent-facing
 * output. Everything an LLM needs to judge "did it render, does it react,
 * did it fault" without looking at an image: region stats, motion score,
 * beat-response ratio, ASCII frame samples, and machine checks (most
 * importantly visibleAfterBrightness — the "renders at 32/255, looks like
 * a crash" trap).
 */

import { createHash } from "node:crypto";
import {
  DEFAULT_BRIGHTNESS_FACTOR,
  DISPLAY_HEIGHT,
  DISPLAY_WIDTH,
  LIVE_MASK,
  LIVE_PIXEL_COUNT,
  toDisplayedFrame,
} from "../core/display";
import { RgbxParamType, f32FromBits } from "../core/abi";
import type { FaultInfo } from "../core/host";
import type { ManifestMetadata } from "../core/manifest";
import type { Scenario } from "../core/scenario";

const ASCII_RAMP = " .:-=+*#%@";

export interface FrameSample {
  tick: number;
  digest: string;
  ascii: string;
  topColors: string[];
}

export interface Check {
  name: string;
  pass: boolean;
  detail?: string;
}

export class RunStats {
  ticks = 0;
  nonBlackTicks = 0;
  firstNonBlackTick = -1;
  visibleAfterBrightnessTicks = 0;
  lumaSum = 0;
  lumaMax = 0;
  noseCutoutWriteTicks = 0;
  manifestViolationTicks = 0;
  goodMomentTicks = 0;
  wallMsAll: number[] = [];
  digests = new Set<string>();
  samples: FrameSample[] = [];
  logLines: string[] = [];

  beatTicks = 0;
  offBeatTicks = 0;
  beatLumaDeltaSum = 0;
  offBeatLumaDeltaSum = 0;

  private prevFrame: Uint8Array | null = null;
  private motionSum = 0;

  private regionSums = {
    leftLens: { luma: 0, r: 0, g: 0, b: 0, n: 0 },
    rightLens: { luma: 0, r: 0, g: 0, b: 0, n: 0 },
    topRow: { luma: 0, n: 0 },
    bottomRow: { luma: 0, n: 0 },
  };

  constructor(private readonly sampleTicks: Set<number>) {}

  /** Splits one drained printk buffer into tagged lines, capped at 500 total
   * so a chatty extension can't grow the report without bound. */
  private pushLog(tag: string, log: string): void {
    if (log.length === 0) {
      return;
    }
    for (const line of log.split("\n")) {
      if (line.length > 0 && this.logLines.length < 500) {
        this.logLines.push(`[${tag}] ${line}`);
      }
    }
  }

  /** printk emitted by rgbx_init, drained from SimHost.initLog after
   * activate(). Tagged `[init]` rather than `[tick N]` because it happens
   * before tick 0 — an extension whose only logging is in init (hello) would
   * otherwise show an empty printk section in the report. */
  recordInitLog(log: string): void {
    this.pushLog("init", log);
  }

  /** printk from a tick that FAULTED. record() never runs for such a tick —
   * there is no framebuffer to account — but its log is the most valuable
   * line in the report: the worker drains it precisely because traps often
   * follow logs. Without this the one diagnostic explaining the trap is the
   * one line missing. Tagged `[tick N]` like any other tick. */
  recordFaultLog(tick: number, log: string): void {
    this.pushLog(`tick ${tick}`, log);
  }

  record(tick: number, frame: Uint8Array, wallMs: number, beatMask: number,
         goodMoment: boolean, manifestIntact: boolean, log: string): void {
    this.ticks++;
    this.wallMsAll.push(wallMs);
    if (goodMoment) {
      this.goodMomentTicks++;
    }
    if (!manifestIntact) {
      this.manifestViolationTicks++;
    }
    this.pushLog(`tick ${tick}`, log);

    let nonBlack = false;
    let cutoutWrite = false;
    let lumaTickSum = 0;
    for (let i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
      const r = frame[i * 3];
      const g = frame[i * 3 + 1];
      const b = frame[i * 3 + 2];
      const lit = (r | g | b) !== 0;
      if (LIVE_MASK[i] === 0) {
        if (lit) {
          cutoutWrite = true;
        }
        continue;
      }
      const luma = (r + g + b) / 3;
      lumaTickSum += luma;
      if (lit) {
        nonBlack = true;
      }
      const x = i % DISPLAY_WIDTH;
      const y = Math.trunc(i / DISPLAY_WIDTH);
      const lens = x < DISPLAY_WIDTH / 2 ? this.regionSums.leftLens : this.regionSums.rightLens;
      lens.luma += luma;
      lens.r += r;
      lens.g += g;
      lens.b += b;
      lens.n++;
      if (y === 0) {
        this.regionSums.topRow.luma += luma;
        this.regionSums.topRow.n++;
      }
      if (y === DISPLAY_HEIGHT - 1) {
        this.regionSums.bottomRow.luma += luma;
        this.regionSums.bottomRow.n++;
      }
      if (luma > this.lumaMax) {
        this.lumaMax = luma;
      }
    }
    const avgLuma = lumaTickSum / LIVE_PIXEL_COUNT;
    this.lumaSum += avgLuma;
    if (nonBlack) {
      this.nonBlackTicks++;
      if (this.firstNonBlackTick < 0) {
        this.firstNonBlackTick = tick;
      }
    }
    if (cutoutWrite) {
      this.noseCutoutWriteTicks++;
    }

    // Post-brightness visibility: does the device's ×0.02+truncate leave
    // anything lit at all?
    const displayed = toDisplayedFrame(frame, DEFAULT_BRIGHTNESS_FACTOR);
    if (displayed.some((v) => v !== 0)) {
      this.visibleAfterBrightnessTicks++;
    }

    // Motion + beat response, from per-tick luma delta vs previous frame.
    if (this.prevFrame !== null) {
      let deltaSum = 0;
      for (let i = 0; i < frame.length; i++) {
        deltaSum += Math.abs(frame[i] - this.prevFrame[i]);
      }
      const normalized = deltaSum / (frame.length * 255);
      this.motionSum += normalized;
      const lumaDelta = deltaSum / frame.length;
      if (beatMask !== 0) {
        this.beatTicks++;
        this.beatLumaDeltaSum += lumaDelta;
      } else {
        this.offBeatTicks++;
        this.offBeatLumaDeltaSum += lumaDelta;
      }
    }
    this.prevFrame = frame.slice();

    const digest = "sha256:" + createHash("sha256").update(frame).digest("hex").slice(0, 32);
    this.digests.add(digest);
    if (this.sampleTicks.has(tick)) {
      this.samples.push({ tick, digest, ascii: frameToAscii(frame), topColors: topColors(frame) });
    }
  }

  motionScore(): number {
    return this.ticks > 1 ? this.motionSum / (this.ticks - 1) : 0;
  }

  beatResponse(): { onBeat: number; offBeat: number; ratio: number; detected: boolean } {
    const onBeat = this.beatTicks > 0 ? this.beatLumaDeltaSum / this.beatTicks : 0;
    const offBeat = this.offBeatTicks > 0 ? this.offBeatLumaDeltaSum / this.offBeatTicks : 0;
    const ratio = offBeat > 1e-6 ? onBeat / offBeat : onBeat > 1e-6 ? Infinity : 0;
    return { onBeat, offBeat, ratio, detected: this.beatTicks > 0 && ratio > 1.5 };
  }

  wallStats(): { min: number; avg: number; max: number; p99: number } {
    if (this.wallMsAll.length === 0) {
      return { min: 0, avg: 0, max: 0, p99: 0 };
    }
    const sorted = [...this.wallMsAll].sort((a, b) => a - b);
    return {
      min: sorted[0],
      avg: sorted.reduce((a, b) => a + b, 0) / sorted.length,
      max: sorted[sorted.length - 1],
      p99: sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * 0.99))],
    };
  }

  regions(): Record<string, unknown> {
    const lens = (s: { luma: number; r: number; g: number; b: number; n: number }) => ({
      avgLuma: s.n > 0 ? round2(s.luma / s.n) : 0,
      dominantChannel:
        s.r >= s.g && s.r >= s.b ? "r" : s.g >= s.b ? "g" : "b",
    });
    return {
      leftLens: lens(this.regionSums.leftLens),
      rightLens: lens(this.regionSums.rightLens),
      topRow: { avgLuma: this.regionSums.topRow.n > 0 ? round2(this.regionSums.topRow.luma / this.regionSums.topRow.n) : 0 },
      bottomRow: { avgLuma: this.regionSums.bottomRow.n > 0 ? round2(this.regionSums.bottomRow.luma / this.regionSums.bottomRow.n) : 0 },
      noseCutoutWrites: this.noseCutoutWriteTicks,
    };
  }
}

function round2(v: number): number {
  return Math.round(v * 100) / 100;
}

/** 40x12 luma render over the live mask (dead cells blank) — LLM-readable. */
export function frameToAscii(frame: Uint8Array): string {
  const rows: string[] = [];
  for (let y = 0; y < DISPLAY_HEIGHT; y++) {
    let row = "";
    for (let x = 0; x < DISPLAY_WIDTH; x++) {
      const i = y * DISPLAY_WIDTH + x;
      if (LIVE_MASK[i] === 0) {
        row += " ";
        continue;
      }
      const luma = (frame[i * 3] + frame[i * 3 + 1] + frame[i * 3 + 2]) / 3;
      const idx = Math.min(ASCII_RAMP.length - 1, Math.trunc((luma / 256) * ASCII_RAMP.length));
      row += ASCII_RAMP[idx];
    }
    rows.push(row);
  }
  return rows.join("\n");
}

/** Up to 3 most-frequent lit colors as #rrggbb. */
export function topColors(frame: Uint8Array): string[] {
  const counts = new Map<number, number>();
  for (let i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
    if (LIVE_MASK[i] === 0) {
      continue;
    }
    const c = (frame[i * 3] << 16) | (frame[i * 3 + 1] << 8) | frame[i * 3 + 2];
    if (c !== 0) {
      counts.set(c, (counts.get(c) ?? 0) + 1);
    }
  }
  return [...counts.entries()]
    .sort((a, b) => b[1] - a[1])
    .slice(0, 3)
    .map(([c]) => "#" + c.toString(16).padStart(6, "0"));
}

/** 24-bit-color ANSI half-block render (two rows per text line). */
export function frameToAnsi(frame: Uint8Array): string {
  const lines: string[] = [];
  for (let y = 0; y < DISPLAY_HEIGHT; y += 2) {
    let line = "";
    for (let x = 0; x < DISPLAY_WIDTH; x++) {
      const top = y * DISPLAY_WIDTH + x;
      const bot = (y + 1) * DISPLAY_WIDTH + x;
      const t = LIVE_MASK[top] === 1
        ? [frame[top * 3], frame[top * 3 + 1], frame[top * 3 + 2]]
        : null;
      const b = LIVE_MASK[bot] === 1
        ? [frame[bot * 3], frame[bot * 3 + 1], frame[bot * 3 + 2]]
        : null;
      if (t === null && b === null) {
        line += "\x1b[0m ";
      } else {
        const tc = t ?? [26, 26, 26]; // dead cell: dark bezel grey
        const bc = b ?? [26, 26, 26];
        line += `\x1b[38;2;${tc[0]};${tc[1]};${tc[2]}m\x1b[48;2;${bc[0]};${bc[1]};${bc[2]}m▀`;
      }
    }
    lines.push(line + "\x1b[0m");
  }
  return lines.join("\n");
}

export interface ReportArgs {
  /** Null when the load itself faulted (bad manifest, missing exports,
   * spinning ctor) — the report still carries result.fault. */
  metadata: ManifestMetadata | null;
  wasmPath: string;
  scenario: Scenario;
  seed: number;
  dtMs: number;
  budgetMs: number;
  stats: RunStats;
  fault: FaultInfo | null;
  checks: Check[];
  hostWallMs: number;
  artifacts: Record<string, string | null>;
}

export function buildReport(args: ReportArgs): Record<string, unknown> {
  const { metadata, stats } = args;
  const beat = stats.beatResponse();
  return {
    schema: "rgbx-sim-report/1",
    extension: {
      name: metadata?.displayName ?? null,
      wasm: args.wasmPath,
      width: metadata?.width ?? null,
      height: metadata?.height ?? null,
      params:
        metadata?.params.map((p, i) => ({
          index: i,
          name: p.name,
          type: ["uint32", "color", "bool", "string", "float32"][p.type],
          default:
            p.type === RgbxParamType.String
              ? metadata.stringDefaults[p.stringSlot]
              : p.type === RgbxParamType.Float
                ? f32FromBits(p.defaultValue)
                : p.defaultValue,
        })) ?? [],
    },
    run: {
      scenario: args.scenario.name,
      seed: args.seed,
      ticks: stats.ticks,
      dtMs: args.dtMs,
      simulatedMs: stats.ticks * args.dtMs,
      hostWallMs: Math.round(args.hostWallMs),
    },
    result: {
      status: args.fault === null ? "ok" : "faulted",
      fault:
        args.fault === null
          ? null
          : {
              tick: args.fault.tick,
              kind: args.fault.kind,
              detail: args.fault.detail,
              paramsResetToDefaults: args.fault.paramsResetToDefaults,
            },
    },
    frames: {
      nonBlackTicks: stats.nonBlackTicks,
      firstNonBlackTick: stats.firstNonBlackTick,
      luma: {
        avg: stats.ticks > 0 ? round2(stats.lumaSum / stats.ticks) : 0,
        max: round2(stats.lumaMax),
      },
      visibleAfterBrightness: stats.visibleAfterBrightnessTicks > 0,
      regions: stats.regions(),
      motionScore: round2(stats.motionScore() * 100) / 100,
      uniqueFrameDigests: stats.digests.size,
      goodMomentTicks: stats.goodMomentTicks,
      manifestViolationTicks: stats.manifestViolationTicks,
      samples: stats.samples,
    },
    audio: {
      beatTicks: stats.beatTicks,
      beatResponse: {
        lumaDeltaOnBeatTicks: round2(beat.onBeat),
        lumaDeltaOffBeat: round2(beat.offBeat),
        ratio: Number.isFinite(beat.ratio) ? round2(beat.ratio) : null,
        detected: beat.detected,
      },
    },
    timing: {
      tickWallMs: (() => {
        const w = stats.wallStats();
        return { min: round2(w.min), avg: round2(w.avg), max: round2(w.max), p99: round2(w.p99) };
      })(),
      budgetMs: args.budgetMs,
      overBudgetTicks: stats.wallMsAll.filter((w) => w > args.budgetMs).length,
    },
    printk: stats.logLines,
    checks: args.checks,
    artifacts: args.artifacts,
  };
}
