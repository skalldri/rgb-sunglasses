import {
  FLOOR_MAX,
  FRAME_MS,
  GATE_MIN,
  GATE_MUSIC_MARGIN,
  GATE_ROOM_MARGIN,
  MAX_TAP_IRREGULARITY,
  MIN_MUSIC_FRAMES,
  MIN_ROOM_FRAMES,
  MIN_TAPS,
  MIN_TARGET_RATIO,
  ROOM_NOISY_RMS,
  TAP_MATCH_MS,
  analyzeMusic,
  analyzeRoom,
  assessTaps,
  classifyTempo,
  matchTaps,
  median,
  percentile,
  reconcileGate,
  replayBeats,
  sweepSensitivity,
  type CalibrationWindow,
} from "@/services/audio-calibration";
import { AUDIO_PARAMS } from "@/services/audio-params";
import { AUDIO_NUM_BANDS } from "@/services/audio-telemetry";

/** Build a window with per-frame control over the fields a given step reads. */
function makeWindow(opts: {
  frames: number;
  rmsInput?: (f: number) => number;
  flux?: (f: number, b: number) => number;
  mean?: (f: number, b: number) => number;
  sigma?: (f: number, b: number) => number;
  clipped?: (f: number) => boolean;
  thresholdMode?: 0 | 1;
  hasStats?: boolean;
  stepMs?: number;
}): CalibrationWindow {
  const stepMs = opts.stepMs ?? FRAME_MS;
  const timeMs: number[] = [];
  const rmsInput: number[] = [];
  const clipped: boolean[] = [];
  const beat: boolean[] = [];
  const flux: number[] = [];
  const mean: number[] = [];
  const sigma: number[] = [];
  for (let f = 0; f < opts.frames; f++) {
    timeMs.push(f * stepMs);
    rmsInput.push(opts.rmsInput?.(f) ?? 0.0005);
    clipped.push(opts.clipped?.(f) ?? false);
    beat.push(false);
    for (let b = 0; b < AUDIO_NUM_BANDS; b++) {
      flux.push(opts.flux?.(f, b) ?? 0);
      mean.push(opts.mean?.(f, b) ?? 0);
      sigma.push(opts.sigma?.(f, b) ?? 0);
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
  };
}

describe("percentile", () => {
  it("interpolates between samples", () => {
    expect(percentile([0, 10], 0.5)).toBeCloseTo(5, 9);
    expect(percentile([0, 10, 20, 30], 0.5)).toBeCloseTo(15, 9);
  });
  it("handles the endpoints and degenerate inputs", () => {
    expect(percentile([5, 1, 9], 0)).toBe(1);
    expect(percentile([5, 1, 9], 1)).toBe(9);
    expect(percentile([7], 0.9)).toBe(7);
    expect(Number.isNaN(percentile([], 0.5))).toBe(true);
  });
  it("does not mutate its input", () => {
    const input = [3, 1, 2];
    percentile(input, 0.5);
    expect(input).toEqual([3, 1, 2]);
  });
  it("median is the 50th percentile", () => {
    expect(median([4, 1, 3, 2])).toBeCloseTo(2.5, 9);
  });
});

describe("step 1 — listen to the room", () => {
  /**
   * Band-0 flux as a SILENT room actually produces it, modeled on the 2026-08-29 bench
   * measurement (987 frames, rms p95 = 0.0006): about half the frames exactly zero — flux is
   * rectified, so any noise loses energy half the time — and the rest log-noise junk reaching
   * past 1.0, p99 = 1.19. Log-flux is a ratio, and near the quantisation floor a tiny
   * absolute change is a huge log change, so silence is SPIKY in flux, not flat.
   */
  const SILENT_ROOM_FLUX = [
    0, 0.71, 0, 0.28, 0, 1.19, 0, 0.09, 0.53, 0, 0.02, 0.4, 0, 0, 1.99, 0,
    0.14, 0, 0.33, 0.05,
  ];

  it("leaves the floor ALONE in a quiet room, however wild the flux looks", () => {
    // THE bug this fit is gated for (2026-08-29, hardware): in a measurably silent room the
    // p99 selects log-noise transients, the wanted floor lands 14x above FLOOR_MAX, and the
    // wizard proposed the MAXIMUM floor justified by "the background noise is loud enough…"
    // while its sibling `noisy` flag said the room was quiet. The flux percentile is only a
    // measurement when the level says there was something to measure.
    const win = makeWindow({
      frames: 100,
      rmsInput: (f) => 0.0002 + (f % 5) * 0.0001, // rms p95 ≈ 0.0006, as measured
      flux: (f, b) => (b === 0 ? SILENT_ROOM_FLUX[f % SILENT_ROOM_FLUX.length] : 0),
    });
    const r = analyzeRoom(win);
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.noisy).toBe(false);
    // No proposal, no saturation, no warning: nothing for the `noisy` flag to contradict.
    expect(r.proposedFloor).toBeNull();
    expect(r.floorSaturated).toBe(false);
    expect(r.warnings).toEqual([]);
    // The junk percentile is still REPORTED (diagnostics), just not fitted to.
    expect(r.quietFlux99).toBeGreaterThan(FLOOR_MAX);
  });

  it("MEASURES a loud room rather than refusing it", () => {
    // The regression this file exists to prevent from coming back. A hard refusal above
    // ROOM_NOISY_RMS locked the wizard out of every real venue: at a festival the crowd never
    // drops below it, so the room step could never pass and no later step could ever run.
    const win = makeWindow({
      frames: 100,
      rmsInput: () => ROOM_NOISY_RMS * 2,
      flux: (_f, b) => (b === 0 ? 0.04 : 0),
    });
    const r = analyzeRoom(win);
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.noisy).toBe(true);
    expect(r.roomP95).toBeCloseTo(ROOM_NOISY_RMS * 2, 6);
    // It still produces the thing a loud room most needs: a fitted beat floor, clearing the
    // room's own worst flux so room noise cannot fire beats.
    expect(r.proposedFloor).not.toBeNull();
    expect(r.proposedFloor as number).toBeGreaterThan(r.quietFlux99);
    expect(r.proposedFloor as number).toBeCloseTo(1.2 * 0.04, 6);
    expect(r.floorSaturated).toBe(false);
    expect(r.warnings.join(" ")).toContain("background noise");
  });

  it("stays quiet about a genuinely quiet room", () => {
    const win = makeWindow({
      frames: 100,
      rmsInput: () => 0.0005,
      flux: (_f, b) => (b === 0 ? 0.01 : 0),
    });
    const r = analyzeRoom(win);
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.noisy).toBe(false);
    expect(r.floorSaturated).toBe(false);
    expect(r.warnings).toEqual([]);
  });

  it("treats the noisy bound as strictly above — AT the bound is still quiet", () => {
    // Documents the boundary so a future >= rewrite fails a test instead of silently moving
    // which rooms get fitted.
    const win = makeWindow({
      frames: 100,
      rmsInput: () => ROOM_NOISY_RMS,
      flux: (_f, b) => (b === 0 ? 5 : 0),
    });
    const r = analyzeRoom(win);
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.noisy).toBe(false);
    expect(r.proposedFloor).toBeNull();
  });

  it("reports the floor saturating instead of exceeding the evidence-backed cap", () => {
    // Above 0.10 the plan doc records real beats being eaten, so a NOISY room whose own flux
    // needs more than that is a room we must WARN about, not one we quietly over-fit. (A
    // quiet room with the same wild flux gets no proposal at all — see the first test.)
    const win = makeWindow({
      frames: 100,
      rmsInput: () => ROOM_NOISY_RMS * 2,
      flux: (_f, b) => (b === 0 ? 5 : 0),
    });
    const r = analyzeRoom(win);
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.noisy).toBe(true);
    expect(r.floorSaturated).toBe(true);
    expect(r.proposedFloor).toBe(FLOOR_MAX);
    expect(r.warnings.join(" ")).toContain("as high as it safely goes");
  });

  it("refuses too short a recording", () => {
    const win = makeWindow({
      frames: MIN_ROOM_FRAMES - 1,
      rmsInput: () => 0.0005,
    });
    expect(analyzeRoom(win).ok).toBe(false);
  });

  it("clamps the proposed floor into the range the plan doc supports", () => {
    const loud = makeWindow({
      frames: 100,
      rmsInput: () => ROOM_NOISY_RMS * 2,
      flux: (_f, b) => (b === 0 ? 5 : 0),
    });
    const r = analyzeRoom(loud);
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    // Above 0.10 the plan doc records real beats being eaten, so the cap holds even when the
    // measurement says otherwise.
    expect(r.proposedFloor).toBeLessThanOrEqual(0.1);

    const noisyButFluxless = makeWindow({
      frames: 100,
      rmsInput: () => ROOM_NOISY_RMS * 2,
      flux: () => 0,
    });
    const r2 = analyzeRoom(noisyButFluxless);
    expect(r2.ok).toBe(true);
    if (!r2.ok) return;
    expect(r2.proposedFloor).toBeGreaterThanOrEqual(0.02);
  });
});

describe("step 2 — let the music play", () => {
  /** A window whose RMS sweeps a realistic dynamic range. */
  function musicWindow(over: Partial<Parameters<typeof makeWindow>[0]> = {}) {
    return makeWindow({
      frames: 200,
      rmsInput: (f) => 0.01 + (f % 50) * 0.0008, // 0.010 .. 0.049
      ...over,
    });
  }

  it("fits a window around the music and keeps the AGC dead band", () => {
    const r = analyzeMusic(musicWindow());
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.targetHigh).toBeGreaterThanOrEqual(
      r.targetLow * MIN_TARGET_RATIO - 1e-9,
    );
    expect(r.targetLow).toBeGreaterThanOrEqual(0.001);
    expect(r.targetHigh).toBeLessThanOrEqual(0.5);
  });

  it("widens a too-narrow window rather than shipping one that oscillates", () => {
    // Compressed music: p25 and p90 nearly equal, so the raw fit is far under 4x. Without the
    // widening the AGC ping-pongs between attack and release forever.
    const r = analyzeMusic(musicWindow({ rmsInput: () => 0.03 }));
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.widened).toBe(true);
    expect(r.targetHigh / r.targetLow).toBeGreaterThanOrEqual(
      MIN_TARGET_RATIO - 1e-6,
    );
  });

  it("widens downward, not upward, so it never chases a clipping mic", () => {
    const r = analyzeMusic(musicWindow({ rmsInput: () => 0.03 }));
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    const unwidenedHigh = Math.min(0.03 * 1.1, 0.5);
    expect(r.targetHigh).toBeCloseTo(unwidenedHigh, 6);
    expect(r.targetLow).toBeLessThan(0.03 * 0.9);
  });

  it("pulls the ceiling down when the mic is clipping, and says so", () => {
    const clean = analyzeMusic(musicWindow());
    const clipping = analyzeMusic(musicWindow({ clipped: (f) => f % 4 === 0 }));
    expect(clean.ok && clipping.ok).toBe(true);
    if (!clean.ok || !clipping.ok) return;
    expect(clipping.clipAdjusted).toBe(true);
    expect(clipping.clipFraction).toBeCloseTo(0.25, 2);
    expect(clipping.targetHigh).toBeLessThan(clean.targetHigh);
  });

  it("does not adjust for a trace of clipping", () => {
    const r = analyzeMusic(musicWindow({ clipped: (f) => f === 0 }));
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.clipAdjusted).toBe(false);
  });

  it("refuses too short a recording", () => {
    expect(analyzeMusic(makeWindow({ frames: MIN_MUSIC_FRAMES - 1 })).ok).toBe(
      false,
    );
  });
});

describe("step 3 — gate reconciliation", () => {
  it("uses the plan doc measurements and lands below quiet music", () => {
    // The measured numbers: quiet-room p95 0.00049, normal-volume music p5 0.00061.
    const g = reconcileGate(0.00049, 0.00061);
    expect(g.noiseGate).toBeLessThan(0.00061);
  });

  it("warns on the plan doc room, because the gate cannot clear it", () => {
    // Only a 1.25x gap between room and music, so the gate that keeps quiet music audible
    // necessarily sits below the room's own noise. That is worth saying out loud rather than
    // silently shipping a gate that lets crowd noise through between songs.
    const g = reconcileGate(0.00049, 0.00061);
    expect(g.noiseGate).toBeLessThan(0.00049);
    expect(g.warning).toContain("nearly as loud");
  });

  it("stays quiet when the room is genuinely quiet", () => {
    const g = reconcileGate(0.0002, 0.01);
    expect(g.warning).toBeNull();
    expect(g.noiseGate).toBeGreaterThan(0.0002);
  });

  it("re-creates the field bug with a generous margin, and does not with 1.15", () => {
    // This is the test that pins the constant. A 1.6x room margin (the intuitive choice)
    // lands ABOVE the music p5 and mutes normal-volume music, which is the original bug.
    const roomP95 = 0.00049;
    const musicP5 = 0.00061;
    expect(1.6 * roomP95).toBeGreaterThan(musicP5); // the bug, if we had chosen 1.6
    expect(GATE_ROOM_MARGIN * roomP95).toBeLessThan(musicP5); // what we chose
  });

  it("takes the music-derived bound when the room is nearly as loud, and warns", () => {
    const g = reconcileGate(0.002, 0.0012);
    expect(g.noiseGate).toBeCloseTo(GATE_MUSIC_MARGIN * 0.0012, 9);
    expect(g.warning).toContain("background noise is nearly as loud");
  });

  it("clamps into the range the firmware actually accepts, not a private ceiling", () => {
    // agcNoiseGateRms accepts 0..0.02. The old bound here was 0.004 — five times tighter than
    // the parameter, for no stated reason — and it is what made a loud room unfittable.
    expect(AUDIO_PARAMS.agcNoiseGateRms.max).toBe(0.02);
    expect(reconcileGate(1, 1).noiseGate).toBeLessThanOrEqual(
      AUDIO_PARAMS.agcNoiseGateRms.max,
    );
    expect(reconcileGate(0, 0).noiseGate).toBeGreaterThanOrEqual(GATE_MIN);
  });

  it("proposes a gate above the old 0.004 ceiling when the venue needs one", () => {
    // A loud room with correspondingly loud music: the gate that belongs here is one the old
    // clamp could not express, so it silently saturated and the room step refused instead.
    const g = reconcileGate(0.01, 0.05);
    expect(g.noiseGate).toBeGreaterThan(0.004);
  });

  it("NEVER lets the gate exceed 80% of the quietest music, however loud the room", () => {
    // This is the property that makes raising the ceiling safe. The ceiling was never what
    // protected the music from being muted -- the min() against the music is. If this ever
    // fails, the original field bug is back: the glasses do nothing until you turn it up.
    for (const roomP95 of [0.0005, 0.003, 0.01, 0.05, 0.5, 5]) {
      for (const musicP5 of [0.0006, 0.002, 0.01, 0.04]) {
        const g = reconcileGate(roomP95, musicP5);
        expect(g.noiseGate).toBeLessThanOrEqual(GATE_MUSIC_MARGIN * musicP5);
      }
    }
  });

  it("warns whenever the chosen gate cannot clear the room, including the venue case", () => {
    // Previously unreachable: analyzeRoom refused above 0.003, so a gate that lost to the
    // room's own noise could never be produced by the loud path this copy describes.
    const g = reconcileGate(0.01, 0.005);
    expect(g.noiseGate).toBeLessThan(0.01);
    expect(g.warning).toContain("nearly as loud");
  });
});

describe("step 4 — tap matching", () => {
  it("recovers a perfect score despite a constant human offset", () => {
    // Humans tap 20-80 ms early or late, consistently. We score AGREEMENT, not alignment,
    // so a consistent offset must not push the fit toward a wrong sensitivity.
    const beats = Array.from({ length: 16 }, (_, i) => i * 500);
    const taps = beats.map((t) => t + 60);
    const m = matchTaps(taps, beats);
    expect(m.offsetMs).toBeCloseTo(60, 6);
    expect(m.f).toBeCloseTo(1, 9);
    expect(m.matched).toBe(16);
  });

  it("handles a negative offset (tapping early)", () => {
    const beats = Array.from({ length: 16 }, (_, i) => i * 500);
    const m = matchTaps(
      beats.map((t) => t - 45),
      beats,
    );
    expect(m.offsetMs).toBeCloseTo(-45, 6);
    expect(m.f).toBeCloseTo(1, 9);
  });

  it("scores partial agreement", () => {
    const beats = Array.from({ length: 10 }, (_, i) => i * 500);
    const taps = beats.filter((_, i) => i % 2 === 0); // caught half
    const m = matchTaps(taps, beats);
    expect(m.recall).toBeCloseTo(1, 9); // every tap matched a beat
    expect(m.precision).toBeCloseTo(0.5, 9); // half the beats went untapped
  });

  it("does not match beyond the radius, even consistently", () => {
    // The offset removal is bounded at the match radius on purpose. Unbounded, it would slide
    // the taps onto the beats however far away they started — so a user tapping the off-beat
    // would score perfectly and the sweep would fit to the wrong sensitivity.
    // 1000 ms spacing with a +240 ms offset, so the nearest beat is unambiguously the one
    // BEFORE the tap (240 away, versus 760 to the next). At 500 ms spacing a +300 offset
    // would really be -200 from the following beat, which measures something else.
    const beats = Array.from({ length: 12 }, (_, i) => i * 1000);
    const m = matchTaps(
      beats.map((t) => t + 240),
      beats,
    );
    expect(m.offsetMs).toBe(TAP_MATCH_MS); // clamped, not the raw 240
    expect(m.matched).toBe(0);
    expect(m.f).toBe(0);
  });

  it("does not absorb a half-beat shift into the offset", () => {
    // This is the 'catching every other beat' failure. If the matcher hid it, classifyTempo
    // would be the only thing left to notice, and the sweep would have already fit to it.
    const beats = Array.from({ length: 12 }, (_, i) => i * 1000);
    const m = matchTaps(
      beats.map((t) => t + 400),
      beats,
    );
    expect(m.f).toBeLessThan(0.5);
  });

  it("never matches one beat to two taps", () => {
    // Double-counting would inflate F and pick an over-sensitive setting.
    const m = matchTaps([0, 10, 20], [5]);
    expect(m.matched).toBe(1);
  });

  it("is empty-safe", () => {
    expect(matchTaps([], [1, 2]).f).toBe(0);
    expect(matchTaps([1, 2], []).f).toBe(0);
  });
});

describe("step 4 — tap quality gate", () => {
  it("accepts steady taps and reports the tempo", () => {
    const taps = Array.from({ length: 16 }, (_, i) => i * 500);
    const q = assessTaps(taps);
    expect(q.ok).toBe(true);
    if (!q.ok) return;
    expect(q.bpm).toBeCloseTo(120, 6);
  });

  it("refuses too few taps rather than fitting to them", () => {
    const q = assessTaps(
      Array.from({ length: MIN_TAPS - 1 }, (_, i) => i * 500),
    );
    expect(q.ok).toBe(false);
    if (q.ok) return;
    expect(q.reason).toContain("left the sensitivity alone");
  });

  it("refuses uneven taps", () => {
    // Alternating 300/700 ms: a median of 500 that describes nothing.
    const taps: number[] = [0];
    for (let i = 1; i < 16; i++)
      taps.push(taps[i - 1] + (i % 2 === 0 ? 300 : 700));
    const q = assessTaps(taps);
    expect(q.ok).toBe(false);
    if (q.ok) return;
    expect(q.reason).toContain("uneven");
  });

  it("tolerates human jitter below the irregularity limit", () => {
    const taps: number[] = [0];
    for (let i = 1; i < 20; i++)
      taps.push(taps[i - 1] + 500 + (i % 3 === 0 ? 25 : -15));
    const q = assessTaps(taps);
    expect(q.ok).toBe(true);
    if (!q.ok) return;
    expect(q.irregularity).toBeLessThan(MAX_TAP_IRREGULARITY);
  });
});

describe("step 4 — classic tempo failures", () => {
  it("detects firing twice per beat and proposes a longer refractory", () => {
    const r = classifyTempo(240, 120);
    expect(r.kind).toBe("double");
    if (r.kind !== "double") return;
    // Just over half a beat: suppresses the extra hit without eating the next real one.
    const beatMs = 500;
    expect(r.proposedRefractoryFrames).toBe(
      Math.round((0.55 * beatMs) / FRAME_MS),
    );
    expect(r.proposedRefractoryFrames * FRAME_MS).toBeGreaterThan(beatMs / 2);
    expect(r.proposedRefractoryFrames * FRAME_MS).toBeLessThan(beatMs);
  });

  it("detects catching every other beat", () => {
    expect(classifyTempo(60, 120).kind).toBe("half");
  });

  it("says nothing when the tempos agree", () => {
    expect(classifyTempo(120, 120).kind).toBe("ok");
    expect(classifyTempo(126, 120).kind).toBe("ok");
  });

  it("does not fire on ratios outside both bands", () => {
    expect(classifyTempo(180, 120).kind).toBe("ok"); // 1.5x
    expect(classifyTempo(360, 120).kind).toBe("ok"); // 3x
  });
});

describe("step 4b — offline sweep", () => {
  /**
   * Build a window with a beat planted every `beatEveryFrames`, where a beat is a flux spike
   * that clears mean + PLANTED_ALPHA*sigma but not much more. A correct sweep should recover
   * an alpha near PLANTED_ALPHA: lower alphas fire on the between-beat noise, higher ones miss.
   */
  const PLANTED_ALPHA = 1.0;
  function plantedWindow(beatEveryFrames = 16, frames = 400) {
    return makeWindow({
      frames,
      mean: () => 0.1,
      sigma: () => 0.1,
      flux: (f, b) => {
        if (b !== 0) return 0;
        const onBeat = f % beatEveryFrames === 0;
        // On a beat: comfortably above mean + 1.0*sigma (0.2). Off a beat: above mean + 0.5*sigma
        // (0.15) but below the planted threshold, so an over-sensitive alpha fires on it.
        return onBeat ? 0.28 : 0.17;
      },
    });
  }

  const CANDIDATES = Array.from({ length: 10 }, (_, i) => ({
    sensitivity: i + 1,
    paramValue: 0.2 + i * 0.2, // 0.2 .. 2.0
  }));

  it("recovers the planted optimum", () => {
    const win = plantedWindow();
    const beatFrames = 16;
    const taps: number[] = [];
    for (let f = 0; f < win.frames; f += beatFrames) taps.push(win.timeMs[f]);

    const r = sweepSensitivity(win, taps, CANDIDATES, 0.05);
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.best.f).toBeGreaterThan(0.9);
    // The planted threshold sits at alpha 1.0; anything much lower fires on the noise floor.
    expect(r.best.paramValue).toBeGreaterThan(0.7);
    expect(r.best.paramValue).toBeLessThanOrEqual(1.1);
  });

  it("evaluates every candidate against every refractory", () => {
    const win = plantedWindow();
    const taps = [win.timeMs[0], win.timeMs[16], win.timeMs[32]];
    const r = sweepSensitivity(win, taps, CANDIDATES, 0.05);
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.evaluated).toBe(CANDIDATES.length * 4);
  });

  it("breaks ties toward the LESS sensitive setting", () => {
    // A false fire strobes the lights on a hi-hat and looks broken; a miss looks lazy. When
    // two settings score identically, the quieter one wins.
    const win = makeWindow({
      frames: 200,
      mean: () => 0.1,
      sigma: () => 0.1,
      flux: (f, b) => (b === 0 && f % 16 === 0 ? 5 : 0), // clears every candidate alpha
    });
    const taps: number[] = [];
    for (let f = 0; f < win.frames; f += 16) taps.push(win.timeMs[f]);
    // Fed MOST-sensitive-first on purpose. In natural order the least sensitive candidate is
    // seen first and wins on iteration order alone, so the test would pass with the tie-break
    // deleted — it did, until this was reversed.
    const r = sweepSensitivity(win, taps, [...CANDIDATES].reverse(), 0.05);
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.best.sensitivity).toBe(1);
  });

  it("refuses without raw statistics", () => {
    // Tier 1 carries no mean/sigma, so a "best" fitted from zeros would be meaningless.
    const win = plantedWindow();
    win.hasStats = false;
    const r = sweepSensitivity(win, [0, 500], CANDIDATES, 0.05);
    expect(r.ok).toBe(false);
    if (r.ok) return;
    expect(r.reason).toContain("could not send the detail");
  });

  it("refuses when nothing matches the taps", () => {
    const win = plantedWindow();
    const r = sweepSensitivity(win, [1e9, 1e9 + 500], CANDIDATES, 0.05);
    expect(r.ok).toBe(false);
  });
});

describe("replayBeats", () => {
  it("honours the absolute floor", () => {
    // A spike that clears the adaptive threshold but not the floor must not fire.
    const win = makeWindow({
      frames: 20,
      mean: () => 0.001,
      sigma: () => 0.001,
      flux: (f, b) => (b === 0 && f === 5 ? 0.01 : 0),
    });
    expect(replayBeats(win, 1, 0, 0.005)).toHaveLength(1);
    expect(replayBeats(win, 1, 0, 0.05)).toHaveLength(0);
  });

  it("suppresses hits inside the refractory window", () => {
    const win = makeWindow({
      frames: 20,
      mean: () => 0.1,
      sigma: () => 0.1,
      flux: (_f, b) => (b === 0 ? 5 : 0), // every frame would fire
    });
    expect(replayBeats(win, 1, 0, 0.05)).toHaveLength(20);
    // With a 4-frame refractory, roughly every 5th frame survives.
    expect(replayBeats(win, 1, 4, 0.05)).toHaveLength(4);
  });

  it("resolves the threshold per mode, matching audio_dsp.cpp", () => {
    // mode 0: mean + alpha*sigma. mode 1: median + delta, where band_mean carries the median
    // and the parameter is added rather than scaled.
    const base = { frames: 10, mean: () => 0.1, sigma: () => 0.1 };
    const spike = (f: number, b: number) => (b === 0 && f === 5 ? 0.25 : 0);

    const mode0 = makeWindow({ ...base, flux: spike, thresholdMode: 0 });
    expect(replayBeats(mode0, 1.0, 0, 0)).toHaveLength(1); // 0.25 > 0.1 + 1.0*0.1
    expect(replayBeats(mode0, 2.0, 0, 0)).toHaveLength(0); // 0.25 < 0.1 + 2.0*0.1

    const mode1 = makeWindow({ ...base, flux: spike, thresholdMode: 1 });
    expect(replayBeats(mode1, 0.1, 0, 0)).toHaveLength(1); // 0.25 > 0.1 + 0.1
    expect(replayBeats(mode1, 0.2, 0, 0)).toHaveLength(0); // 0.25 < 0.1 + 0.2
  });

  it("applies the refractory per band, not globally", () => {
    // Band 1 firing must not suppress band 0's next hit; the firmware tracks them separately.
    const win = makeWindow({
      frames: 10,
      mean: () => 0.1,
      sigma: () => 0.1,
      flux: (f, b) => (b === 1 ? 5 : b === 0 && f === 3 ? 5 : 0),
    });
    const times = replayBeats(win, 1, 2, 0.05);
    // Band 1 fires on frames 0,3,6,9. Band 0 also fires on frame 3 — already in the list.
    expect(times).toContain(win.timeMs[3]);
  });
});
