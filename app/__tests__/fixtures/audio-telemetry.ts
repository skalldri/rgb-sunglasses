/**
 * Byte-level fixtures for the audio telemetry wire format.
 *
 * These build frames from the SPEC (fw/src/sound/audio_telemetry_codec.h), independently of
 * the decoder under test — hand-written offsets and a hand-written quantiser. That is the
 * point: a fixture that reused the decoder's own constants would agree with any layout the
 * decoder happened to implement, including a wrong one. This is the only pre-hardware check
 * that the app and the firmware mean the same thing by "byte 12".
 */

export const FIXTURE_VERSION = 1;
export const BANDS = 4;
export const BUCKETS = 20;

/** Independent reimplementation of the firmware's audio_telemetry_q_log(). */
export function q(v: number): number {
  if (!(v > 0)) return 0;
  const raw = 40 * Math.log10(v) + 200;
  if (raw < 1) return 0;
  if (raw > 255) return 255;
  return Math.floor(raw + 0.5);
}

export type FrameSpec = {
  tier?: number;
  version?: number;
  seq?: number;
  dropped?: number;
  gainSteps?: number;
  rmsInput?: number;
  rmsInstant?: number;
  peak?: number;
  noiseFloor?: number;
  clipCount?: number;
  framesSinceStep?: number;
  silent?: boolean;
  clipped?: boolean;
  agcFrozen?: boolean;
  thresholdMode?: 0 | 1;
  beatMask?: number;
  flux?: number[];
  threshold?: number[];
  mean?: number[];
  sigma?: number[];
  buckets?: number[];
  /** Bytes appended past the tier's size, to prove trailing data is ignored. */
  trailing?: number[];
};

const SIZE = { 1: 20, 2: 28, 3: 48 } as const;

export function makeFrame(spec: FrameSpec = {}): Uint8Array {
  const tier = spec.tier ?? 1;
  const version = spec.version ?? FIXTURE_VERSION;
  const size = (SIZE as Record<number, number>)[tier] ?? 20;
  const trailing = spec.trailing ?? [];
  const out = new Uint8Array(size + trailing.length);

  out[0] = ((version & 0x0f) << 4) | (tier & 0x0f);

  let flags = 0;
  if (spec.silent) flags |= 0x01;
  if (spec.clipped) flags |= 0x02;
  if (spec.agcFrozen) flags |= 0x04;
  if (spec.thresholdMode) flags |= 0x08;
  flags |= ((spec.beatMask ?? 0) & 0x0f) << 4;
  out[1] = flags;

  const seq = spec.seq ?? 0;
  out[2] = seq & 0xff;
  out[3] = (seq >> 8) & 0xff;
  out[4] = spec.dropped ?? 0;
  out[5] = (spec.gainSteps ?? 0) & 0xff;
  out[6] = q(spec.rmsInput ?? 0);
  out[7] = q(spec.rmsInstant ?? 0);
  out[8] = q(spec.peak ?? 0);
  out[9] = q(spec.noiseFloor ?? 0);
  out[10] = spec.clipCount ?? 0;
  out[11] = spec.framesSinceStep ?? 0;

  for (let b = 0; b < BANDS; b++) {
    out[12 + b] = q(spec.flux?.[b] ?? 0);
    out[16 + b] = q(spec.threshold?.[b] ?? 0);
  }
  if (tier >= 2) {
    for (let b = 0; b < BANDS; b++) {
      out[20 + b] = q(spec.mean?.[b] ?? 0);
      out[24 + b] = q(spec.sigma?.[b] ?? 0);
    }
  }
  if (tier >= 3) {
    for (let i = 0; i < BUCKETS; i++) {
      out[28 + i] = q(spec.buckets?.[i] ?? 0);
    }
  }
  for (let i = 0; i < trailing.length; i++) {
    out[size + i] = trailing[i];
  }
  return out;
}

export function toBase64(bytes: Uint8Array): string {
  let s = "";
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return btoa(s);
}

/**
 * A synthetic stream: `seconds` of frames at `rateHz`, with a beat every `bpm`.
 * Returns frames paired with the timestamp they should be pushed at.
 */
export function makeStream(opts: {
  bpm?: number;
  seconds?: number;
  rateHz?: number;
  startMs?: number;
  tier?: number;
  gainSteps?: number;
  noiseFloor?: number;
  rmsInput?: number;
  peak?: number;
  silent?: boolean;
  clipEvery?: number;
}): { bytes: Uint8Array; timeMs: number }[] {
  const rateHz = opts.rateHz ?? 8;
  const seconds = opts.seconds ?? 10;
  const bpm = opts.bpm ?? 120;
  const startMs = opts.startMs ?? 0;
  const stepMs = 1000 / rateHz;
  const beatPeriodMs = 60_000 / bpm;
  const total = Math.round(seconds * rateHz);

  const out: { bytes: Uint8Array; timeMs: number }[] = [];
  let nextBeatMs = 0;
  for (let n = 0; n < total; n++) {
    const timeMs = startMs + n * stepMs;
    const rel = n * stepMs;
    let beatMask = 0;
    if (rel >= nextBeatMs) {
      beatMask = 0x1;
      nextBeatMs += beatPeriodMs;
    }
    out.push({
      timeMs,
      bytes: makeFrame({
        tier: opts.tier ?? 1,
        seq: n * 4,
        gainSteps: opts.gainSteps ?? 0,
        rmsInput: opts.rmsInput ?? 0.02,
        rmsInstant: opts.rmsInput ?? 0.02,
        peak: opts.peak ?? 0.2,
        noiseFloor: opts.noiseFloor ?? 0.0006,
        silent: opts.silent ?? false,
        clipped: opts.clipEvery ? n % opts.clipEvery === 0 : false,
        beatMask,
      }),
    });
  }
  return out;
}
