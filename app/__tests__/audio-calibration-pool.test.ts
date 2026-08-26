/**
 * Tests for opportunistic collection pools.
 *
 * The arithmetic these feed (analyzeRoom, analyzeMusic, sweepSensitivity) is tested elsewhere
 * and unchanged. What is new, and what these cover, is that a pool assembled from SEVERAL
 * separate sittings behaves like one recording — and that the two aggregates which cannot be
 * reversed (hasStats, medianStepMs) survive a discard correctly.
 */

import {
  appendChunk,
  dropLastChunk,
  emptyPool,
  poolSeconds,
  readiness,
  type PoolChunk,
} from "@/services/audio-calibration-pool";
import { analyzeRoom, percentile } from "@/services/audio-calibration";
import { AUDIO_NUM_BANDS } from "@/services/audio-telemetry";

/** A chunk of `frames` frames starting at t0, one frame every stepMs. */
function chunk(opts: {
  frames: number;
  t0: number;
  stepMs?: number;
  rms?: (i: number) => number;
  flux?: (i: number, b: number) => number;
  hasStats?: boolean;
  thresholdMode?: 0 | 1;
}): PoolChunk {
  const stepMs = opts.stepMs ?? 32;
  const timeMs: number[] = [];
  const rmsInput: number[] = [];
  const clipped: boolean[] = [];
  const beat: boolean[] = [];
  const flux: number[] = [];
  const mean: number[] = [];
  const sigma: number[] = [];
  for (let i = 0; i < opts.frames; i++) {
    timeMs.push(opts.t0 + i * stepMs);
    rmsInput.push(opts.rms ? opts.rms(i) : 0.001);
    clipped.push(false);
    beat.push(false);
    for (let b = 0; b < AUDIO_NUM_BANDS; b++) {
      flux.push(opts.flux ? opts.flux(i, b) : 0.01);
      mean.push(0.005);
      sigma.push(0.002);
    }
  }
  return {
    frames: opts.frames,
    timeMs,
    rmsInput,
    clipped,
    beat,
    flux,
    mean,
    sigma,
    thresholdMode: opts.thresholdMode ?? 0,
    hasStats: opts.hasStats ?? true,
    medianStepMs: stepMs,
  };
}

describe("pool accumulation", () => {
  it("starts empty and ready for nothing", () => {
    const p = emptyPool();
    expect(p.frames).toBe(0);
    expect(readiness(p, 30).ready).toBe(false);
    // An empty pool is not a DECIMATED pool. Seeding hasStats false would make the collector
    // read "this link cannot do it" when the truth is "nothing collected yet".
    expect(p.hasStats).toBe(true);
  });

  it("concatenates several sittings into one window", () => {
    let p = emptyPool();
    p = appendChunk(p, chunk({ frames: 20, t0: 1000 }));
    p = appendChunk(p, chunk({ frames: 15, t0: 60_000 }));
    p = appendChunk(p, chunk({ frames: 25, t0: 200_000 }));

    expect(p.frames).toBe(60);
    expect(p.timeMs.length).toBe(60);
    expect(p.flux.length).toBe(60 * AUDIO_NUM_BANDS);
    expect(p.chunks.length).toBe(3);
    // Chronology within the pool is preserved across a three-minute gap.
    expect(p.timeMs[0]).toBe(1000);
    expect(p.timeMs[59]).toBe(200_000 + 24 * 32);
  });

  it("ignores a chunk that captured no frames", () => {
    let p = appendChunk(emptyPool(), chunk({ frames: 10, t0: 0 }));
    p = appendChunk(p, chunk({ frames: 0, t0: 5000 }));
    expect(p.frames).toBe(10);
    // No empty chunk recorded, so a later discard removes REAL data rather than wasting
    // itself on a phantom.
    expect(p.chunks.length).toBe(1);
  });

  it("counts seconds per chunk, not across the gaps between them", () => {
    let p = emptyPool();
    p = appendChunk(p, chunk({ frames: 33, t0: 0, stepMs: 32 })); // ~1.02 s
    p = appendChunk(p, chunk({ frames: 33, t0: 600_000, stepMs: 32 })); // ten minutes later
    // A pool gathered over ten minutes holds two seconds of audio, and the readout has to say
    // two seconds — otherwise "9s / 8s needed" is met by waiting rather than by collecting.
    expect(poolSeconds(p)).toBeCloseTo(2.048, 2);
  });

  it("feeds the existing analysis unchanged", () => {
    // The point of concatenation: a pool IS a CalibrationWindow. Percentiles over rmsInput are
    // order-independent, so a room measured in three lulls equals one measured in one.
    let split = emptyPool();
    split = appendChunk(split, chunk({ frames: 40, t0: 0, rms: () => 0.0005 }));
    split = appendChunk(
      split,
      chunk({ frames: 40, t0: 100_000, rms: () => 0.0005 }),
    );
    const single = appendChunk(
      emptyPool(),
      chunk({ frames: 80, t0: 0, rms: () => 0.0005 }),
    );

    const a = analyzeRoom(split);
    const b = analyzeRoom(single);
    expect(a.ok).toBe(true);
    expect(b.ok).toBe(true);
    if (!a.ok || !b.ok) return;
    expect(a.roomP95).toBeCloseTo(b.roomP95, 9);
    expect(a.proposedFloor).toBeCloseTo(b.proposedFloor, 9);
  });
});

describe("pool aggregates that cannot be reversed", () => {
  it("one decimated chunk makes the whole pool unfit for the sweep", () => {
    let p = appendChunk(emptyPool(), chunk({ frames: 30, t0: 0 }));
    expect(p.hasStats).toBe(true);
    p = appendChunk(p, chunk({ frames: 30, t0: 5000, hasStats: false }));
    // AND, not OR: the sweep replays every frame, so any decimated stretch poisons it.
    expect(p.hasStats).toBe(false);
  });

  it("takes the WORST frame spacing, not the average of them", () => {
    let p = appendChunk(emptyPool(), chunk({ frames: 30, t0: 0, stepMs: 32 }));
    p = appendChunk(p, chunk({ frames: 30, t0: 9000, stepMs: 125 }));
    // 125 ms is the 8 Hz tier. Averaging would report ~78 ms and quietly pass a guard whose
    // whole job is to refuse fitting a frame-counted refractory against decimated frames.
    expect(p.medianStepMs).toBe(125);
  });

  it("RESTORES fitness when the offending chunk is discarded", () => {
    let p = appendChunk(emptyPool(), chunk({ frames: 30, t0: 0, stepMs: 32 }));
    p = appendChunk(
      p,
      chunk({ frames: 30, t0: 9000, stepMs: 125, hasStats: false }),
    );
    expect(p.hasStats).toBe(false);
    expect(p.medianStepMs).toBe(125);

    p = dropLastChunk(p);
    expect(p.frames).toBe(30);
    expect(p.hasStats).toBe(true);
    expect(p.medianStepMs).toBe(32);
  });

  it("does NOT rehabilitate a pool whose earlier chunk was decimated", () => {
    // The bug this pins. Recomputing by resetting hasStats to true after a discard looks
    // right and is wrong: the bad chunk is still in the pool, and the sweep would then fit a
    // frame-counted refractory against frames that never had the resolution for one.
    let p = appendChunk(
      emptyPool(),
      chunk({ frames: 30, t0: 0, stepMs: 125, hasStats: false }),
    );
    p = appendChunk(p, chunk({ frames: 30, t0: 9000, stepMs: 32 }));
    p = dropLastChunk(p); // throw away the GOOD one

    expect(p.frames).toBe(30);
    expect(p.hasStats).toBe(false);
    expect(p.medianStepMs).toBe(125);
  });
});

describe("discarding a ruined chunk", () => {
  it("costs that chunk and nothing else", () => {
    let p = emptyPool();
    p = appendChunk(p, chunk({ frames: 40, t0: 0, rms: () => 0.0005 }));
    p = appendChunk(p, chunk({ frames: 40, t0: 50_000, rms: () => 0.0005 }));
    // The band came back early during the third sitting.
    p = appendChunk(p, chunk({ frames: 40, t0: 90_000, rms: () => 0.05 }));

    const contaminated = percentile(p.rmsInput, 0.95);
    p = dropLastChunk(p);
    const cleaned = percentile(p.rmsInput, 0.95);

    expect(p.frames).toBe(80);
    expect(p.chunks.length).toBe(2);
    expect(contaminated).toBeGreaterThan(0.01);
    expect(cleaned).toBeCloseTo(0.0005, 6);
    // Every array is trimmed together — a band array left long would silently misalign every
    // frame's flux with the wrong timestamp.
    expect(p.timeMs.length).toBe(80);
    expect(p.flux.length).toBe(80 * AUDIO_NUM_BANDS);
    expect(p.mean.length).toBe(80 * AUDIO_NUM_BANDS);
    expect(p.sigma.length).toBe(80 * AUDIO_NUM_BANDS);
  });

  it("is a no-op on an empty pool and clears a single-chunk one", () => {
    expect(dropLastChunk(emptyPool()).frames).toBe(0);
    const one = appendChunk(emptyPool(), chunk({ frames: 12, t0: 0 }));
    const gone = dropLastChunk(one);
    expect(gone.frames).toBe(0);
    expect(gone.chunks.length).toBe(0);
    expect(gone.flux.length).toBe(0);
  });

  it("never mutates the pool it was given", () => {
    const one = appendChunk(emptyPool(), chunk({ frames: 12, t0: 0 }));
    const two = appendChunk(one, chunk({ frames: 12, t0: 5000 }));
    dropLastChunk(two);
    expect(one.frames).toBe(12);
    expect(two.frames).toBe(24);
  });
});

describe("readiness", () => {
  it("reports progress toward the minimum without capping at it", () => {
    let p = emptyPool();
    p = appendChunk(p, chunk({ frames: 20, t0: 0 }));
    expect(readiness(p, 30)).toMatchObject({ frames: 20, ready: false });

    p = appendChunk(p, chunk({ frames: 40, t0: 9000 }));
    // Topping up past the minimum is allowed and improves the fit — a collector that refused
    // more would make a marginal sample permanent.
    expect(readiness(p, 30)).toMatchObject({
      frames: 60,
      ready: true,
      chunks: 2,
    });
  });

  it("tracks the newest threshold shape, since the user can change it mid-session", () => {
    let p = appendChunk(emptyPool(), chunk({ frames: 10, t0: 0, thresholdMode: 0 }));
    expect(p.thresholdMode).toBe(0);
    p = appendChunk(p, chunk({ frames: 10, t0: 5000, thresholdMode: 1 }));
    expect(p.thresholdMode).toBe(1);
  });
});
