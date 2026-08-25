import { AUDIO_NUM_BANDS } from "@/services/audio-telemetry";

/**
 * The guided calibration wizard's arithmetic. Every step is a pure function so it can be
 * tested against synthetic rooms without a venue, a board, or a phone.
 *
 * THE GOVERNING RULE IS THAT EVERY STEP MUST BE ABLE TO REFUSE. A wizard that always produces
 * an answer will confidently fit garbage — a room measured while the support act is playing, a
 * user who tapped four times and gave up — and then write that fit to the device the person is
 * about to wear on stage. Each analyse* function therefore returns a discriminated result with
 * an `ok` flag and a plain-language reason, and the caller is expected to show the reason
 * rather than a number.
 *
 * Constants here are NOT round numbers picked for looks. Where one came from a measurement in
 * docs/plans/2026-08-02-beat-detection-phase3-and-beyond.md it says so, because the difference
 * between 1.15 and 1.6 in the gate reconciliation is the difference between this wizard fixing
 * the field bug and re-creating it.
 */

/* ── shared helpers ── */

/** Linear-interpolated percentile over an UNSORTED array. Returns NaN for an empty input. */
export function percentile(values: number[], p: number): number {
  if (values.length === 0) return NaN;
  const sorted = [...values].sort((a, b) => a - b);
  if (sorted.length === 1) return sorted[0];
  const idx = (sorted.length - 1) * Math.min(Math.max(p, 0), 1);
  const lo = Math.floor(idx);
  const hi = Math.ceil(idx);
  if (lo === hi) return sorted[lo];
  return sorted[lo] + (sorted[hi] - sorted[lo]) * (idx - lo);
}

export function median(values: number[]): number {
  return percentile(values, 0.5);
}

function clamp(v: number, lo: number, hi: number): number {
  if (!Number.isFinite(v)) return lo;
  return v < lo ? lo : v > hi ? hi : v;
}

/* ── recorded window ── */

/**
 * A recorded stretch of telemetry, already dequantised.
 *
 * `flux`, `mean` and `sigma` are flat, frame-major arrays of length frames*AUDIO_NUM_BANDS —
 * the same layout the ring uses, so extracting one costs a copy and no restructuring.
 */
export type CalibrationWindow = {
  frames: number;
  timeMs: number[];
  /** Input-referred smoothed RMS: the noise gate's own comparand, and what the AGC targets. */
  rmsInput: number[];
  clipped: boolean[];
  beat: boolean[];
  flux: number[];
  mean: number[];
  sigma: number[];
  /** 0 = mean + alpha*sigma, 1 = median + delta. Decides how the sweep resolves a threshold. */
  thresholdMode: 0 | 1;
  /** True when every frame carried raw stats (tier 2+). The sweep is meaningless without them. */
  hasStats: boolean;
};

/* ── step 1: listen to the room ── */

/** Above this the room is too loud to be measuring a noise floor in. */
export const ROOM_TOO_LOUD_RMS = 0.003;
/** Minimum frames before a room measurement is believed (~4 s at 8 Hz). */
export const MIN_ROOM_FRAMES = 30;

export type RoomResult =
  | { ok: false; reason: string }
  | {
      ok: true;
      roomP95: number;
      quietFlux99: number;
      /** Proposed Minimum beat strength (beatFluxFloor). */
      proposedFloor: number;
    };

export function analyzeRoom(win: CalibrationWindow): RoomResult {
  if (win.frames < MIN_ROOM_FRAMES) {
    return {
      ok: false,
      reason: "I did not hear enough to measure the room. Try again.",
    };
  }
  const roomP95 = percentile(win.rmsInput, 0.95);
  if (!Number.isFinite(roomP95)) {
    return {
      ok: false,
      reason: "I did not hear enough to measure the room. Try again.",
    };
  }
  if (roomP95 > ROOM_TOO_LOUD_RMS) {
    return {
      ok: false,
      reason:
        "It's not quiet enough to measure the room. Try again between songs.",
    };
  }

  /* Band 0 only: the kick band is where a false fire is most visible, and it is the band the
   * lights follow. The floor applies to every band, so fitting it to the noisiest-in-practice
   * band is the conservative choice. */
  const band0: number[] = [];
  for (let f = 0; f < win.frames; f++)
    band0.push(win.flux[f * AUDIO_NUM_BANDS]);
  const quietFlux99 = percentile(band0, 0.99);

  /* 1.2x the loudest thing the quiet room produced, so room noise cannot clear the floor.
   * Capped at 0.10 because the plan doc records real beats being eaten above that. */
  const proposedFloor = clamp(1.2 * quietFlux99, 0.02, 0.1);
  return { ok: true, roomP95, quietFlux99, proposedFloor };
}

/* ── step 2: let the music play ── */

/** Minimum frames of music before the AGC window is fitted (~8 s at 8 Hz). */
export const MIN_MUSIC_FRAMES = 60;
/** Above this fraction of clipping, back the ceiling off. */
export const MUSIC_CLIP_FRACTION = 0.02;
/**
 * The AGC needs a dead band between "turn it down" and "turn it up" or it oscillates between
 * attack and release forever. 4x (12 dB) is the narrowest that held in the plan doc's replays.
 */
export const MIN_TARGET_RATIO = 4;

export type MusicResult =
  | { ok: false; reason: string }
  | {
      ok: true;
      targetLow: number;
      targetHigh: number;
      musicP5: number;
      /** True when the window had to be widened to reach MIN_TARGET_RATIO. */
      widened: boolean;
      /** True when the ceiling was pulled down because the mic was clipping. */
      clipAdjusted: boolean;
      clipFraction: number;
    };

export function analyzeMusic(win: CalibrationWindow): MusicResult {
  if (win.frames < MIN_MUSIC_FRAMES) {
    return {
      ok: false,
      reason: "I need a bit more music to work with. Try again.",
    };
  }
  const p25 = percentile(win.rmsInput, 0.25);
  const p90 = percentile(win.rmsInput, 0.9);
  const musicP5 = percentile(win.rmsInput, 0.05);
  if (!Number.isFinite(p25) || !Number.isFinite(p90)) {
    return {
      ok: false,
      reason: "I need a bit more music to work with. Try again.",
    };
  }

  const clipFraction = win.clipped.filter(Boolean).length / win.frames;

  let low = clamp(p25 * 0.9, 0.001, 0.1);
  let high = clamp(p90 * 1.1, 0.02, 0.5);

  let clipAdjusted = false;
  if (clipFraction > MUSIC_CLIP_FRACTION) {
    high = clamp(high * 0.75, 0.02, 0.5);
    clipAdjusted = true;
  }

  /* Widen DOWNWARD, not upward: raising the ceiling toward a clipping mic is the wrong
   * direction, and the floor has far more room before it hits its own clamp. */
  let widened = false;
  if (high < low * MIN_TARGET_RATIO) {
    const wanted = high / MIN_TARGET_RATIO;
    const newLow = clamp(wanted, 0.001, 0.1);
    if (newLow < low) {
      low = newLow;
      widened = true;
    } else {
      /* The floor is already at its clamp, so the only way to open the band is upward. */
      high = clamp(low * MIN_TARGET_RATIO, 0.02, 0.5);
      widened = true;
    }
  }

  return {
    ok: true,
    targetLow: low,
    targetHigh: high,
    musicP5,
    widened,
    clipAdjusted,
    clipFraction,
  };
}

/* ── step 3: gate reconciliation ── */

/**
 * 1.15, NOT 1.6.
 *
 * The plan doc's own measurements are quiet-room p95 = 0.00049 against normal-volume music
 * p5 = 0.00061 — a gap of only 1.25x. A generous margin above the room therefore lands ABOVE
 * quiet music and re-creates the exact field bug this whole feature exists to fix ("the
 * glasses do nothing until you turn the volume up"). The `min()` against 0.8*musicP5 is the
 * real guard; this multiplier only keeps the gate off the room's own noise.
 */
export const GATE_ROOM_MARGIN = 1.15;
export const GATE_MUSIC_MARGIN = 0.8;

export type GateResult = {
  noiseGate: number;
  /** Set when the room is nearly as loud as the music, so the gate cannot separate them. */
  warning: string | null;
};

export function reconcileGate(roomP95: number, musicP5: number): GateResult {
  const fromRoom = GATE_ROOM_MARGIN * roomP95;
  const fromMusic = GATE_MUSIC_MARGIN * musicP5;
  const noiseGate = clamp(Math.min(fromRoom, fromMusic), 0.0002, 0.004);

  /* Warn on the condition that actually matters — the chosen gate does not clear the room's
   * own noise, so room noise will sometimes get through — rather than on which of the two
   * bounds won. Those are nearly the same test, but this one is the thing the user will
   * observe, and it stays meaningful if either margin is ever retuned. */
  const warning =
    noiseGate < roomP95
      ? "This room's background noise is nearly as loud as the music, so the gate has been set from the music instead. Expect the lights to react to crowd noise between songs."
      : null;
  return { noiseGate, warning };
}

/* ── step 4: tap along ── */

/** Nearest-neighbour match radius. Wider than this stops being "the same beat". */
export const TAP_MATCH_MS = 100;
export const MIN_TAPS = 8;
/** Above this, the taps were too uneven to fit anything to. */
export const MAX_TAP_IRREGULARITY = 0.35;

export type TapMatch = {
  /** Constant human offset removed before scoring. Humans tap 20-80 ms early or late. */
  offsetMs: number;
  matched: number;
  precision: number;
  recall: number;
  f: number;
};

/**
 * Score how well a set of detected beats agrees with a set of taps.
 *
 * The constant offset is removed first, deliberately: we are scoring AGREEMENT, not absolute
 * alignment. A user who taps a consistent 60 ms late is hearing the same beats the detector
 * is, and penalising that would push the fit toward a wrong sensitivity.
 */
export function matchTaps(tapTimes: number[], beatTimes: number[]): TapMatch {
  if (tapTimes.length === 0 || beatTimes.length === 0) {
    return { offsetMs: 0, matched: 0, precision: 0, recall: 0, f: 0 };
  }

  /* Offset from the nearest neighbour of each tap, before any matching decision. */
  const deltas: number[] = [];
  for (const t of tapTimes) {
    let best = Infinity;
    for (const b of beatTimes) {
      const d = t - b;
      if (Math.abs(d) < Math.abs(best)) best = d;
    }
    if (Number.isFinite(best)) deltas.push(best);
  }
  /* BOUNDED. Removing an unbounded offset makes the score meaningless: it would slide the
   * taps onto the beats no matter how far away they started, so a user tapping the off-beat
   * — or the 'catching every other beat' failure this wizard exists to detect — would score a
   * perfect match and the sweep would happily fit to it. The plan's own figure for a human
   * offset is 20-80 ms; anything past the match radius is not an offset, it is a different
   * beat, and it should cost recall rather than being absorbed. */
  const rawOffset = deltas.length > 0 ? median(deltas) : 0;
  const offsetMs = Math.max(-TAP_MATCH_MS, Math.min(TAP_MATCH_MS, rawOffset));

  const shifted = tapTimes.map((t) => t - offsetMs);
  const usedBeat = new Array<boolean>(beatTimes.length).fill(false);
  let matched = 0;
  for (const t of shifted) {
    let bestIdx = -1;
    let bestDist = Infinity;
    for (let i = 0; i < beatTimes.length; i++) {
      if (usedBeat[i]) continue;
      const d = Math.abs(t - beatTimes[i]);
      if (d < bestDist) {
        bestDist = d;
        bestIdx = i;
      }
    }
    if (bestIdx >= 0 && bestDist <= TAP_MATCH_MS) {
      usedBeat[bestIdx] = true;
      matched++;
    }
  }

  const precision = beatTimes.length > 0 ? matched / beatTimes.length : 0;
  const recall = tapTimes.length > 0 ? matched / tapTimes.length : 0;
  const f =
    precision + recall > 0
      ? (2 * precision * recall) / (precision + recall)
      : 0;
  return { offsetMs, matched, precision, recall, f };
}

export type TapQuality =
  | { ok: false; reason: string }
  | { ok: true; medianIntervalMs: number; bpm: number; irregularity: number };

/** Reject taps that are too few or too uneven to fit a sensitivity to. */
export function assessTaps(tapTimes: number[]): TapQuality {
  if (tapTimes.length < MIN_TAPS) {
    return {
      ok: false,
      reason: `I only caught ${tapTimes.length} taps, so I've left the sensitivity alone. You can nudge it by hand.`,
    };
  }
  const sorted = [...tapTimes].sort((a, b) => a - b);
  const intervals: number[] = [];
  for (let i = 1; i < sorted.length; i++)
    intervals.push(sorted[i] - sorted[i - 1]);
  const med = median(intervals);
  if (!(med > 0)) {
    return {
      ok: false,
      reason: "Your taps were too close together to read. Try again.",
    };
  }
  const iqr = percentile(intervals, 0.75) - percentile(intervals, 0.25);
  const irregularity = iqr / med;
  if (irregularity > MAX_TAP_IRREGULARITY) {
    return {
      ok: false,
      reason:
        "Your taps were a bit uneven, so I've left the sensitivity alone. You can nudge it by hand.",
    };
  }
  return { ok: true, medianIntervalMs: med, bpm: 60_000 / med, irregularity };
}

export type TempoRelation =
  | { kind: "ok" }
  | { kind: "double"; message: string; proposedRefractoryFrames: number }
  | { kind: "half"; message: string };

/** Analysis frame period, ~31.25 Hz. Refractory is counted in frames, not milliseconds. */
export const FRAME_MS = 32;

/**
 * Detect the two classic failures by comparing tempos rather than individual beats.
 *
 * These are worth naming explicitly because both look like "it's wrong" to a user but need
 * OPPOSITE fixes: firing twice per beat needs a longer refractory, catching every other beat
 * needs more sensitivity. Guessing between them wastes a lot of venue time.
 */
export function classifyTempo(
  detectedBpm: number,
  tappedBpm: number,
): TempoRelation {
  if (!(detectedBpm > 0) || !(tappedBpm > 0)) return { kind: "ok" };
  const ratio = detectedBpm / tappedBpm;
  if (ratio >= 1.8 && ratio <= 2.2) {
    const tapIntervalMs = 60_000 / tappedBpm;
    return {
      kind: "double",
      message: "It's firing twice per beat.",
      /* Just over half a beat, so the second hit is suppressed and the next real one is not. */
      proposedRefractoryFrames: Math.max(
        1,
        Math.round((0.55 * tapIntervalMs) / FRAME_MS),
      ),
    };
  }
  if (ratio >= 0.45 && ratio <= 0.55) {
    return { kind: "half", message: "It's catching every other beat." };
  }
  return { kind: "ok" };
}

/* ── step 4b: offline sensitivity sweep ── */

export const SWEEP_REFRACTORY_CANDIDATES = [3, 5, 8, 12] as const;

export type SweepCandidate = {
  sensitivity: number;
  /** alpha (mode 0) or delta (mode 1) — whichever the recorded window's mode uses. */
  paramValue: number;
  refractoryFrames: number;
  f: number;
  precision: number;
  recall: number;
};

export type SweepResult =
  | { ok: false; reason: string }
  | { ok: true; best: SweepCandidate; evaluated: number };

/**
 * Replay a recorded window against candidate sensitivities and refractory settings.
 *
 * This is why tier 2 telemetry carries raw mean/sigma: the resolved threshold on the wire has
 * the floor already folded in and cannot be inverted back, so evaluating a DIFFERENT alpha
 * needs the raw statistics the detector used. It is also why the tap step asks for undecimated
 * frames — the refractory counts frames, so a decimated window would model a longer refractory
 * than the device will actually apply.
 *
 * The fire test mirrors audio_dsp.cpp: mode 0 is mean + alpha*sigma, mode 1 is median + delta
 * (where band_mean carries the median and delta replaces alpha). MUST TRACK that file.
 */
export function sweepSensitivity(
  win: CalibrationWindow,
  tapTimes: number[],
  candidateValues: { sensitivity: number; paramValue: number }[],
  floor: number,
): SweepResult {
  if (!win.hasStats) {
    return {
      ok: false,
      reason:
        "This link could not send the detail needed to try other settings.",
    };
  }
  if (win.frames < 2 || tapTimes.length === 0 || candidateValues.length === 0) {
    return {
      ok: false,
      reason: "Not enough recorded audio to try other settings.",
    };
  }

  let best: SweepCandidate | null = null;
  let evaluated = 0;

  for (const cand of candidateValues) {
    for (const refractory of SWEEP_REFRACTORY_CANDIDATES) {
      const beatTimes = replayBeats(win, cand.paramValue, refractory, floor);
      const score = matchTaps(tapTimes, beatTimes);
      evaluated++;
      const entry: SweepCandidate = {
        sensitivity: cand.sensitivity,
        paramValue: cand.paramValue,
        refractoryFrames: refractory,
        f: score.f,
        precision: score.precision,
        recall: score.recall,
      };
      if (
        best === null ||
        entry.f > best.f + 1e-9 ||
        /* Tie-break toward the LESS sensitive setting. A false fire is far more visible on
         * stage than a miss: the lights strobe on a hi-hat and the whole thing looks broken,
         * where a missed beat just looks slightly lazy. */
        (Math.abs(entry.f - best.f) <= 1e-9 &&
          entry.sensitivity < best.sensitivity)
      ) {
        best = entry;
      }
    }
  }

  if (best === null || best.f <= 0) {
    return {
      ok: false,
      reason:
        "I couldn't match your taps to anything the glasses heard. Sensitivity left alone.",
    };
  }
  return { ok: true, best, evaluated };
}

/** Replay the detector over a recorded window. Exported for tests; not part of the flow. */
export function replayBeats(
  win: CalibrationWindow,
  paramValue: number,
  refractoryFrames: number,
  floor: number,
): number[] {
  const out: number[] = [];
  const cooldown = new Array<number>(AUDIO_NUM_BANDS).fill(0);

  for (let f = 0; f < win.frames; f++) {
    let fired = false;
    for (let b = 0; b < AUDIO_NUM_BANDS; b++) {
      const i = f * AUDIO_NUM_BANDS + b;
      if (cooldown[b] > 0) {
        cooldown[b]--;
        continue;
      }
      const threshold =
        win.thresholdMode === 1
          ? win.mean[i] + paramValue
          : win.mean[i] + paramValue * win.sigma[i];
      const flux = win.flux[i];
      if (flux > floor && flux > threshold) {
        cooldown[b] = refractoryFrames;
        fired = true;
      }
    }
    if (fired) out.push(win.timeMs[f]);
  }
  return out;
}

/* ── the proposal ── */

export type ProposedChange = {
  key: string;
  /** Human label, taken from the caller's param table so this file owns no copy of it. */
  label: string;
  oldValue: number;
  newValue: number;
  /** Why this changed, in one sentence. Shown per-row in the review step. */
  because: string;
};

export type CalibrationProposal = {
  changes: ProposedChange[];
  warnings: string[];
  notes: string[];
};
