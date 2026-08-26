import { decodeBytesFromBase64 } from "@/services/ble-value-codec";

/**
 * Decoder and ring buffer for the live audio telemetry stream (firmware service 9).
 *
 * THE WIRE FORMAT IS OWNED BY THE FIRMWARE, at fw/src/sound/audio_telemetry_codec.h. Every
 * constant below is a mirror of one there, and the offsets are named identically so the two
 * files grep against each other. This is a second implementation, not a shared one — the
 * firmware header is C and compiled into the image, so there is nothing to import — which
 * means the only thing keeping them honest is that both sides are versioned and both sides
 * are tested against hand-built byte arrays. A frame carrying an unrecognised version is
 * DISCARDED, never guessed at.
 *
 * QUANTISATION: every magnitude is one byte on a 0.5 dB ladder, costing ~6% relative error.
 * That is invisible on a meter. It is NOT adequate to re-derive a beat decision from, so
 * this module never recomputes `flux > threshold` — the authoritative per-band beat bits
 * ride in the flags byte and are the only thing the UI may treat as "a beat happened".
 */

export const AUDIO_TELEMETRY_VERSION = 1;
export const AUDIO_NUM_BANDS = 4;
export const AUDIO_NUM_DISPLAY_BUCKETS = 20;

export const TELEMETRY_TIER_OFF = 0;
export const TELEMETRY_TIER_METERS = 1;
export const TELEMETRY_TIER_STATS = 2;
export const TELEMETRY_TIER_SPECTRUM = 3;

export type TelemetryTier =
  | typeof TELEMETRY_TIER_OFF
  | typeof TELEMETRY_TIER_METERS
  | typeof TELEMETRY_TIER_STATS
  | typeof TELEMETRY_TIER_SPECTRUM;

/* Byte offsets — mirrors of AUDIO_TELEMETRY_OFF_* in the firmware header. */
const OFF_HEADER = 0;
const OFF_FLAGS = 1;
const OFF_SEQ = 2;
const OFF_DROPPED = 4;
const OFF_GAIN = 5;
const OFF_RMS_IN = 6;
const OFF_RMS_INST = 7;
const OFF_PEAK = 8;
const OFF_NOISE = 9;
const OFF_CLIPS = 10;
const OFF_SINCE_STEP = 11;
const OFF_FLUX = 12;
const OFF_THRESHOLD = OFF_FLUX + AUDIO_NUM_BANDS;
const OFF_MEAN = OFF_THRESHOLD + AUDIO_NUM_BANDS;
const OFF_SIGMA = OFF_MEAN + AUDIO_NUM_BANDS;
const OFF_BUCKETS = OFF_SIGMA + AUDIO_NUM_BANDS;

export const TIER_SIZE_METERS = OFF_THRESHOLD + AUDIO_NUM_BANDS; // 20
export const TIER_SIZE_STATS = OFF_BUCKETS; // 28
export const TIER_SIZE_SPECTRUM = OFF_BUCKETS + AUDIO_NUM_DISPLAY_BUCKETS; // 48

/* flags byte */
const FLAG_SILENT = 0x01;
const FLAG_CLIPPED = 0x02;
const FLAG_AGC_FROZEN = 0x04;
const FLAG_THRESHOLD_MODE = 0x08;
const BEAT_SHIFT = 4;

/**
 * Wire size of a payload tier, or 0 if it is not a payload tier.
 *
 * Note tier 1 is exactly 20 bytes because that is ATT MTU 23 minus the 3-byte notify header
 * — so meters keep flowing on a link that never negotiated an MTU, which is a real and
 * durable state on the OnePlus after a GATT-changing reflash (issue #115).
 */
export function telemetryTierSize(tier: number): number {
  switch (tier) {
    case TELEMETRY_TIER_METERS:
      return TIER_SIZE_METERS;
    case TELEMETRY_TIER_STATS:
      return TIER_SIZE_STATS;
    case TELEMETRY_TIER_SPECTRUM:
      return TIER_SIZE_SPECTRUM;
    default:
      return 0;
  }
}

/**
 * Inverse of the firmware's audio_telemetry_q_log(): q == 0 means "zero or below the floor",
 * and q in [1, 255] encodes 20*log10(v) = q/2 - 100 dB.
 *
 * Uses Math.pow where the firmware iterates a multiplication ladder (float pow is compiled
 * out firmware-wide). The two therefore differ by the ladder's accumulated rounding — under
 * 3e-5 relative across the whole range, which is three orders of magnitude below the 6%
 * quantisation error already baked into q. Matching the firmware's iteration bit-for-bit
 * would make this slower and no more correct.
 */
export function dequantiseLog(q: number): number {
  if (q === 0) {
    return 0;
  }
  return Math.pow(10, (q - 200) / 40);
}

/** dBFS for a normalised magnitude. Zero maps to the meter's floor, not -Infinity. */
export const TELEMETRY_DB_FLOOR = -100;
export function magnitudeToDb(v: number): number {
  if (!(v > 0)) {
    return TELEMETRY_DB_FLOOR;
  }
  const db = 20 * Math.log10(v);
  return db < TELEMETRY_DB_FLOOR ? TELEMETRY_DB_FLOOR : db;
}

export type TelemetryFrame = {
  /** The tier the firmware ACTUALLY sent, read from the header — never what we asked for. */
  tier: TelemetryTier;
  seq: number;
  /** Wrapping count of firmware ticks that carried no new DSP frame. Not a transport counter. */
  dropped: number;
  /** PDM gain relative to the 0 dB park, in 0.5 dB register steps. Lossless on the wire. */
  gainSteps: number;
  gainDb: number;

  rmsInput: number;
  rmsInstant: number;
  peak: number;
  noiseFloor: number;

  clipCount: number;
  framesSinceStep: number;

  silent: boolean;
  clipped: boolean;
  agcFrozen: boolean;
  /** 0 = mean + alpha*sigma, 1 = median + delta. Labels the threshold; does not change it. */
  thresholdMode: 0 | 1;
  /** Authoritative. Bit b set means band b fired since the previous send (sticky-OR). */
  beatMask: number;
  beats: boolean[];

  flux: number[];
  /** The effective fire line the detector applied, already resolved for mode and floor. */
  threshold: number[];

  /** Tier 2+. Raw statistics for the calibration wizard's offline replay; null at tier 1. */
  mean: number[] | null;
  sigma: number[] | null;
  /** Tier 3 only. */
  buckets: number[] | null;
};

function readBand(bytes: Uint8Array, base: number): number[] {
  const out = new Array<number>(AUDIO_NUM_BANDS);
  for (let b = 0; b < AUDIO_NUM_BANDS; b++) {
    out[b] = dequantiseLog(bytes[base + b]);
  }
  return out;
}

/**
 * Decode one notification payload.
 *
 * Returns null for anything it cannot parse with certainty: wrong version, a tier the header
 * does not name, or a buffer shorter than that tier requires. Trailing bytes beyond the
 * tier's size are IGNORED rather than rejected — that is what lets a future firmware add a
 * tier 4 without this build going blind, since tiers are byte-exact prefixes of each other.
 */
export function decodeTelemetryFrame(
  bytes: Uint8Array | null | undefined,
): TelemetryFrame | null {
  if (!bytes || bytes.length < 1) {
    return null;
  }
  const header = bytes[OFF_HEADER];
  if (header >> 4 !== AUDIO_TELEMETRY_VERSION) {
    return null;
  }
  const tier = header & 0x0f;
  const size = telemetryTierSize(tier);
  if (size === 0 || bytes.length < size) {
    return null;
  }

  const flags = bytes[OFF_FLAGS];
  const beatMask = (flags >> BEAT_SHIFT) & 0x0f;
  const beats = new Array<boolean>(AUDIO_NUM_BANDS);
  for (let b = 0; b < AUDIO_NUM_BANDS; b++) {
    beats[b] = (beatMask & (1 << b)) !== 0;
  }

  /* int8: the firmware writes a signed step count through a uint8 cast. */
  const gainRaw = bytes[OFF_GAIN];
  const gainSteps = gainRaw > 127 ? gainRaw - 256 : gainRaw;

  const frame: TelemetryFrame = {
    tier: tier as TelemetryTier,
    seq: bytes[OFF_SEQ] | (bytes[OFF_SEQ + 1] << 8),
    dropped: bytes[OFF_DROPPED],
    gainSteps,
    gainDb: gainSteps * 0.5,
    rmsInput: dequantiseLog(bytes[OFF_RMS_IN]),
    rmsInstant: dequantiseLog(bytes[OFF_RMS_INST]),
    peak: dequantiseLog(bytes[OFF_PEAK]),
    noiseFloor: dequantiseLog(bytes[OFF_NOISE]),
    clipCount: bytes[OFF_CLIPS],
    framesSinceStep: bytes[OFF_SINCE_STEP],
    silent: (flags & FLAG_SILENT) !== 0,
    clipped: (flags & FLAG_CLIPPED) !== 0,
    agcFrozen: (flags & FLAG_AGC_FROZEN) !== 0,
    thresholdMode: (flags & FLAG_THRESHOLD_MODE) !== 0 ? 1 : 0,
    beatMask,
    beats,
    flux: readBand(bytes, OFF_FLUX),
    threshold: readBand(bytes, OFF_THRESHOLD),
    mean: null,
    sigma: null,
    buckets: null,
  };

  if (tier >= TELEMETRY_TIER_STATS) {
    frame.mean = readBand(bytes, OFF_MEAN);
    frame.sigma = readBand(bytes, OFF_SIGMA);
  }
  if (tier >= TELEMETRY_TIER_SPECTRUM) {
    const buckets = new Array<number>(AUDIO_NUM_DISPLAY_BUCKETS);
    for (let i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) {
      buckets[i] = dequantiseLog(bytes[OFF_BUCKETS + i]);
    }
    frame.buckets = buckets;
  }
  return frame;
}

/**
 * Read just the spectrum buckets out of a payload, without building a whole frame object.
 *
 * The spectrum is display-only — it never enters the ring, because the calibration wizard
 * replays flux/mean/sigma and nothing reads a stored spectrum back. This exists so the
 * notification path can write the bars straight to shared values without hand-copying the
 * bucket offset, which would be a second definition of the layout living in a UI file.
 *
 * @param out pre-allocated array of AUDIO_NUM_DISPLAY_BUCKETS, written in place.
 * @returns the largest bucket magnitude, or -1 if this payload carries no spectrum.
 */
export function decodeBucketsInto(
  bytes: Uint8Array | null | undefined,
  out: number[],
): number {
  if (!bytes || bytes.length < TIER_SIZE_SPECTRUM) {
    return -1;
  }
  if (bytes[OFF_HEADER] >> 4 !== AUDIO_TELEMETRY_VERSION) {
    return -1;
  }
  if ((bytes[OFF_HEADER] & 0x0f) < TELEMETRY_TIER_SPECTRUM) {
    return -1;
  }
  let max = 0;
  for (let i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) {
    const v = dequantiseLog(bytes[OFF_BUCKETS + i]);
    out[i] = v;
    if (v > max) max = v;
  }
  return max;
}

/** Convenience for the monitor callback, which receives base64 from react-native-ble-plx. */
export function decodeTelemetryFrameFromBase64(
  value?: string | null,
): TelemetryFrame | null {
  return decodeTelemetryFrame(decodeBytesFromBase64(value));
}

/* ────────────────────────── ring buffer ──────────────────────────
 *
 * The stream runs at up to 32 Hz and must never cause a React render on the fast path, so
 * this stores RAW QUANTISED BYTES in preallocated typed arrays and dequantises on read.
 * Nothing is allocated per frame.
 *
 * Keeping the bytes rather than decoded floats is not just an allocation trick: the
 * calibration wizard (phase 6) replays a recorded window against candidate sensitivities and
 * needs the mean/sigma history it was actually sent, at full fidelity. Storing floats would
 * cost 4x the memory to represent the same 256 distinct values.
 */

/**
 * Sized for the WIZARD'S TAP STEP, which is the longest window anything extracts: 30 s at the
 * undecimated 32 Hz the tap step requests is 960 frames.
 *
 * 512 was wrong and only looked right because requestStream was inert, leaving the stream at
 * 8 Hz. At a working 32 Hz, tapping slower than ~84 BPM — or letting the step run its full 30 s
 * instead of finishing early at 24 taps — wrapped the ring and silently dropped the head of the
 * window, while tapsRef kept every tap. Those orphaned early taps could then never match a
 * beat, capping recall for every candidate in the sweep and quietly biasing the fit.
 *
 * ~31 KB of typed arrays at this size, which is nothing on a phone.
 */
export const RING_FRAMES = 1024;

export type TelemetryRing = {
  capacity: number;
  /** Total frames ever pushed; the ring holds the last min(count, capacity). */
  count: number;
  timeMs: Float64Array;
  seq: Uint16Array;
  dropped: Uint8Array;
  flags: Uint8Array;
  gain: Int8Array;
  rmsIn: Uint8Array;
  rmsInst: Uint8Array;
  peak: Uint8Array;
  noise: Uint8Array;
  clips: Uint8Array;
  sinceStep: Uint8Array;
  tier: Uint8Array;
  flux: Uint8Array; // capacity * AUDIO_NUM_BANDS
  threshold: Uint8Array;
  mean: Uint8Array;
  sigma: Uint8Array;
};

export function createTelemetryRing(
  capacity: number = RING_FRAMES,
): TelemetryRing {
  const bands = capacity * AUDIO_NUM_BANDS;
  return {
    capacity,
    count: 0,
    timeMs: new Float64Array(capacity),
    seq: new Uint16Array(capacity),
    dropped: new Uint8Array(capacity),
    flags: new Uint8Array(capacity),
    gain: new Int8Array(capacity),
    rmsIn: new Uint8Array(capacity),
    rmsInst: new Uint8Array(capacity),
    peak: new Uint8Array(capacity),
    noise: new Uint8Array(capacity),
    clips: new Uint8Array(capacity),
    sinceStep: new Uint8Array(capacity),
    tier: new Uint8Array(capacity),
    flux: new Uint8Array(bands),
    threshold: new Uint8Array(bands),
    mean: new Uint8Array(bands),
    sigma: new Uint8Array(bands),
  };
}

export function resetTelemetryRing(ring: TelemetryRing): void {
  ring.count = 0;
}

/**
 * Decode a payload straight into the ring, without building an intermediate object.
 *
 * @returns true if the frame was accepted. A rejected frame leaves the ring untouched, so a
 *          corrupt notification cannot poison the history the wizard later replays.
 */
export function pushTelemetryBytes(
  ring: TelemetryRing,
  bytes: Uint8Array | null | undefined,
  timeMs: number,
): boolean {
  if (!bytes || bytes.length < 1) {
    return false;
  }
  const header = bytes[OFF_HEADER];
  if (header >> 4 !== AUDIO_TELEMETRY_VERSION) {
    return false;
  }
  const tier = header & 0x0f;
  const size = telemetryTierSize(tier);
  if (size === 0 || bytes.length < size) {
    return false;
  }

  const i = ring.count % ring.capacity;
  ring.timeMs[i] = timeMs;
  ring.seq[i] = bytes[OFF_SEQ] | (bytes[OFF_SEQ + 1] << 8);
  ring.dropped[i] = bytes[OFF_DROPPED];
  ring.flags[i] = bytes[OFF_FLAGS];
  ring.gain[i] = bytes[OFF_GAIN]; // Int8Array reinterprets the byte as signed
  ring.rmsIn[i] = bytes[OFF_RMS_IN];
  ring.rmsInst[i] = bytes[OFF_RMS_INST];
  ring.peak[i] = bytes[OFF_PEAK];
  ring.noise[i] = bytes[OFF_NOISE];
  ring.clips[i] = bytes[OFF_CLIPS];
  ring.sinceStep[i] = bytes[OFF_SINCE_STEP];
  ring.tier[i] = tier;

  const base = i * AUDIO_NUM_BANDS;
  for (let b = 0; b < AUDIO_NUM_BANDS; b++) {
    ring.flux[base + b] = bytes[OFF_FLUX + b];
    ring.threshold[base + b] = bytes[OFF_THRESHOLD + b];
    /* Tier 1 carries no raw stats. Zeroed rather than left stale, so a replay over a window
     * that spans a tier change cannot silently mix this frame's flux with a previous
     * frame's mean — q == 0 already means "zero or below the floor" and reads as absent. */
    ring.mean[base + b] =
      tier >= TELEMETRY_TIER_STATS ? bytes[OFF_MEAN + b] : 0;
    ring.sigma[base + b] =
      tier >= TELEMETRY_TIER_STATS ? bytes[OFF_SIGMA + b] : 0;
  }

  ring.count++;
  return true;
}

/** Ring index of the nth-most-recent frame (0 = newest), or -1 if it is not held. */
export function ringIndex(ring: TelemetryRing, fromNewest: number): number {
  const held = Math.min(ring.count, ring.capacity);
  if (fromNewest < 0 || fromNewest >= held) {
    return -1;
  }
  return (ring.count - 1 - fromNewest + ring.capacity * 2) % ring.capacity;
}

/* ────────────────────────── summary ──────────────────────────
 *
 * The ONLY thing React ever sees from the stream. Recomputed at 2 Hz over a rolling window,
 * so a 32 Hz stream drives at most 2 renders/s of the scoreboard and none at all of the
 * meters (those read shared values on the UI thread).
 */

/** Gain step bounds, mirroring AgcController::kGainMin/kGainMax relative to kGainPark. */
export const GAIN_STEPS_MIN = -40; // -20 dB
export const GAIN_STEPS_MAX = 40; // +20 dB

/**
 * Ratio at which a band bar is drawn full, and the clamp applied to every band ratio.
 *
 * Lives here rather than in either component because THREE places consume it: the provider
 * (clamping what it writes to the shared values), the bars (positioning the fire tick), and
 * the monitor panel's accessibility pass. When it lived in two of them the bar could pin at
 * full while the label announced "480 percent of the firing level" — the same tick meaning
 * two different things.
 */
export const BAND_RATIO_MAX = 1.5;

export const SUMMARY_WINDOW_MS = 10_000;
/** Beyond this with no frame, the meters freeze rather than decay. See VerdictBanner. */
export const STALE_AFTER_MS = 1_000;

export type TelemetrySummary = {
  /** Frames held in the window. Zero means we have nothing to say, not "silence". */
  frames: number;
  /** True when a frame arrived recently enough to believe. */
  live: boolean;
  ageMs: number;
  tier: TelemetryTier;

  gainSteps: number;
  gainDb: number;
  gainPinnedHigh: boolean;
  gainPinnedLow: boolean;
  /** AGC gain changes within the window — how busy the loop is. */
  gainChanges: number;

  rmsInputDb: number;
  noiseFloorDb: number;
  peakDb: number;
  /** dB between the loudest recent peak and full scale. */
  /**
   * dB between the loudest recent peak and full scale, or null when the peak sits at the
   * meter's floor — i.e. every frame in the window quantised to zero. Negating the floor
   * sentinel produced a confident "+100 dB of headroom" on every quiet room, which reads as
   * a measurement rather than as the absence of one.
   */
  headroomDb: number | null;

  silentFraction: number;
  clipFraction: number;
  beatsPerSecond: number;
  /** null when there are too few beats to say anything honest. */
  bpm: number | null;
  /** Lowest band that fired on the most recent beat, or null. */
  lastBeatBand: number | null;
  thresholdMode: 0 | 1;

  /** Firmware ticks in the window that carried no new DSP frame. */
  droppedInWindow: number;
  agcFrozen: boolean;
};

export const EMPTY_SUMMARY: TelemetrySummary = {
  frames: 0,
  live: false,
  ageMs: Number.POSITIVE_INFINITY,
  tier: TELEMETRY_TIER_OFF,
  gainSteps: 0,
  gainDb: 0,
  gainPinnedHigh: false,
  gainPinnedLow: false,
  gainChanges: 0,
  rmsInputDb: TELEMETRY_DB_FLOOR,
  noiseFloorDb: TELEMETRY_DB_FLOOR,
  peakDb: TELEMETRY_DB_FLOOR,
  headroomDb: null,
  silentFraction: 0,
  clipFraction: 0,
  beatsPerSecond: 0,
  bpm: null,
  lastBeatBand: null,
  thresholdMode: 0,
  droppedInWindow: 0,
  agcFrozen: false,
};

/** Minimum beats before a BPM is offered. Below this the median is noise wearing a number. */
const MIN_BEATS_FOR_BPM = 4;
/** Plausible musical tempo. Outside this the estimate is reported as null, not clamped. */
const BPM_MIN = 40;
const BPM_MAX = 220;

export function summarizeTelemetry(
  ring: TelemetryRing,
  nowMs: number,
  windowMs: number = SUMMARY_WINDOW_MS,
): TelemetrySummary {
  const held = Math.min(ring.count, ring.capacity);
  if (held === 0) {
    return EMPTY_SUMMARY;
  }

  const newest = ringIndex(ring, 0);
  const ageMs = nowMs - ring.timeMs[newest];

  let frames = 0;
  let silent = 0;
  let clipped = 0;
  let beatFrames = 0;
  let dropped = 0;
  let gainChanges = 0;
  let peakQ = 0;
  let firstTimeMs = ring.timeMs[newest];
  let lastGain: number | null = null;
  let lastBeatBand: number | null = null;

  /* Beat times, newest-first, for the BPM median. Bounded by the window, and a beat frame is
   * at most one per send, so this cannot grow past the ring. */
  const beatTimes: number[] = [];

  for (let n = 0; n < held; n++) {
    const i = ringIndex(ring, n);
    const t = ring.timeMs[i];
    if (nowMs - t > windowMs) {
      break;
    }
    frames++;
    firstTimeMs = t;

    const flags = ring.flags[i];
    if (flags & FLAG_SILENT) silent++;
    if (flags & FLAG_CLIPPED) clipped++;
    if (ring.peak[i] > peakQ) peakQ = ring.peak[i];

    const mask = (flags >> BEAT_SHIFT) & 0x0f;
    if (mask !== 0) {
      beatFrames++;
      beatTimes.push(t);
      if (lastBeatBand === null) {
        /* Lowest set bit: the kick band is what the lights should follow when several fire. */
        for (let b = 0; b < AUDIO_NUM_BANDS; b++) {
          if (mask & (1 << b)) {
            lastBeatBand = b;
            break;
          }
        }
      }
    }

    const g = ring.gain[i];
    if (lastGain !== null && g !== lastGain) gainChanges++;
    lastGain = g;

    /* `dropped` is a wrapping counter, so accumulate deltas between adjacent sends rather
     * than differencing the endpoints — which would read 0 across a full wrap. */
    if (n > 0) {
      const prev = ringIndex(ring, n - 1);
      dropped += (ring.dropped[prev] - ring.dropped[i] + 256) % 256;
    }
  }

  const spanS = frames > 1 ? (ring.timeMs[newest] - firstTimeMs) / 1000 : 0;
  const beatsPerSecond = spanS > 0 ? beatFrames / spanS : 0;

  let bpm: number | null = null;
  if (beatTimes.length >= MIN_BEATS_FOR_BPM) {
    /* MEAN interval across the whole window, not the median of individual intervals. Each
     * interval carries up to one send-period of quantisation error, and those errors are
     * zero-mean, so averaging over the window cancels most of them; a median just picks one
     * quantised value and reports it with full confidence. At the default 8 Hz a 461 ms pulse
     * (130 BPM) is only ever observable as 375 or 500 ms, and the median estimator turned a
     * steady 130 BPM into exactly "120". */
    const newestBeat = beatTimes[0];
    const oldestBeat = beatTimes[beatTimes.length - 1];
    const spanMs = newestBeat - oldestBeat;
    const gaps = beatTimes.length - 1;
    if (spanMs > 0 && gaps > 0) {
      const candidate = 60_000 / (spanMs / gaps);
      bpm =
        candidate >= BPM_MIN && candidate <= BPM_MAX
          ? Math.round(candidate)
          : null;
    }
  }

  const gainSteps = ring.gain[newest];
  const peakMag = dequantiseLog(peakQ);
  const flags = ring.flags[newest];

  return {
    frames,
    live: ageMs <= STALE_AFTER_MS,
    ageMs,
    tier: ring.tier[newest] as TelemetryTier,
    gainSteps,
    gainDb: gainSteps * 0.5,
    gainPinnedHigh: gainSteps >= GAIN_STEPS_MAX,
    gainPinnedLow: gainSteps <= GAIN_STEPS_MIN,
    gainChanges,
    rmsInputDb: magnitudeToDb(dequantiseLog(ring.rmsIn[newest])),
    noiseFloorDb: magnitudeToDb(dequantiseLog(ring.noise[newest])),
    peakDb: magnitudeToDb(peakMag),
    /* Headroom is derived, never a wire field: how far the loudest recent peak sits below
     * full scale. Negative would mean the peak itself exceeded 0 dBFS. */
    headroomDb: peakQ === 0 ? null : -magnitudeToDb(peakMag),
    silentFraction: frames > 0 ? silent / frames : 0,
    clipFraction: frames > 0 ? clipped / frames : 0,
    beatsPerSecond,
    bpm,
    lastBeatBand,
    thresholdMode: (flags & FLAG_THRESHOLD_MODE) !== 0 ? 1 : 0,
    droppedInWindow: dropped,
    agcFrozen: (flags & FLAG_AGC_FROZEN) !== 0,
  };
}

/**
 * Copy a time range out of the ring as a dequantised calibration window.
 *
 * The wizard replays this against candidate settings, so two properties matter more than
 * they look. It reports `hasStats` false if ANY frame in the range lacked raw statistics —
 * a window that is 90% tier 2 is still not replayable, because the missing frames would read
 * as zero-valued statistics and score as beats that never happened. And it refuses a range
 * spanning a send-rate change by reporting the observed frame spacing, so the caller can tell
 * whether the refractory (counted in frames) means what it thinks.
 */
export function extractCalibrationWindow(
  ring: TelemetryRing,
  fromMs: number,
  toMs: number,
): {
  frames: number;
  timeMs: number[];
  rmsInput: number[];
  clipped: boolean[];
  beat: boolean[];
  flux: number[];
  mean: number[];
  sigma: number[];
  thresholdMode: 0 | 1;
  hasStats: boolean;
  medianStepMs: number;
} {
  const held = Math.min(ring.count, ring.capacity);
  const timeMs: number[] = [];
  const rmsInput: number[] = [];
  const clipped: boolean[] = [];
  const beat: boolean[] = [];
  const flux: number[] = [];
  const mean: number[] = [];
  const sigma: number[] = [];
  let hasStats = true;
  let thresholdMode: 0 | 1 = 0;

  /* Walk oldest-first so the output is chronological, which the replay depends on. */
  for (let n = held - 1; n >= 0; n--) {
    const i = ringIndex(ring, n);
    if (i < 0) continue;
    const t = ring.timeMs[i];
    if (t < fromMs || t > toMs) continue;

    timeMs.push(t);
    rmsInput.push(dequantiseLog(ring.rmsIn[i]));
    clipped.push((ring.flags[i] & FLAG_CLIPPED) !== 0);
    beat.push(((ring.flags[i] >> BEAT_SHIFT) & 0x0f) !== 0);
    thresholdMode = (ring.flags[i] & FLAG_THRESHOLD_MODE) !== 0 ? 1 : 0;
    if (ring.tier[i] < TELEMETRY_TIER_STATS) hasStats = false;

    const base = i * AUDIO_NUM_BANDS;
    for (let b = 0; b < AUDIO_NUM_BANDS; b++) {
      flux.push(dequantiseLog(ring.flux[base + b]));
      mean.push(dequantiseLog(ring.mean[base + b]));
      sigma.push(dequantiseLog(ring.sigma[base + b]));
    }
  }

  const steps: number[] = [];
  for (let k = 1; k < timeMs.length; k++) steps.push(timeMs[k] - timeMs[k - 1]);
  steps.sort((a, b) => a - b);
  const medianStepMs = steps.length > 0 ? steps[Math.floor(steps.length / 2)] : 0;

  return {
    frames: timeMs.length,
    timeMs,
    rmsInput,
    clipped,
    beat,
    flux,
    mean,
    sigma,
    thresholdMode,
    hasStats: hasStats && timeMs.length > 0,
    medianStepMs,
  };
}
