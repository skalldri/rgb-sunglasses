import {
  AUDIO_NUM_BANDS,
  AUDIO_NUM_DISPLAY_BUCKETS,
  EMPTY_SUMMARY,
  GAIN_STEPS_MAX,
  TELEMETRY_DB_FLOOR,
  TELEMETRY_TIER_METERS,
  TELEMETRY_TIER_SPECTRUM,
  TELEMETRY_TIER_STATS,
  TIER_SIZE_METERS,
  TIER_SIZE_SPECTRUM,
  TIER_SIZE_STATS,
  createTelemetryRing,
  decodeTelemetryFrame,
  decodeTelemetryFrameFromBase64,
  dequantiseLog,
  magnitudeToDb,
  pushTelemetryBytes,
  ringIndex,
  summarizeTelemetry,
  telemetryTierSize,
} from "@/services/audio-telemetry";
import { makeFrame, makeStream, q, toBase64 } from "./fixtures/audio-telemetry";

describe("wire constants", () => {
  it("pins the three tier sizes as wire contract", () => {
    // These are the numbers the firmware static_asserts. If either side moves without a
    // version bump, frames decode into garbage rather than being rejected.
    expect(TIER_SIZE_METERS).toBe(20);
    expect(TIER_SIZE_STATS).toBe(28);
    expect(TIER_SIZE_SPECTRUM).toBe(48);
    expect(AUDIO_NUM_BANDS).toBe(4);
    expect(AUDIO_NUM_DISPLAY_BUCKETS).toBe(20);
  });

  it("keeps the meters tier inside an unnegotiated ATT MTU", () => {
    // ATT_MTU 23 - 3 bytes of notify header. This is the whole reason tier 1 exists: on the
    // OnePlus stale-GATT split-brain the link sits at MTU 23 indefinitely, and
    // bt_gatt_notify() cannot fragment.
    expect(TIER_SIZE_METERS).toBeLessThanOrEqual(20);
  });

  it("reports tier sizes and rejects non-payload tiers", () => {
    expect(telemetryTierSize(TELEMETRY_TIER_METERS)).toBe(20);
    expect(telemetryTierSize(TELEMETRY_TIER_STATS)).toBe(28);
    expect(telemetryTierSize(TELEMETRY_TIER_SPECTRUM)).toBe(48);
    expect(telemetryTierSize(0)).toBe(0);
    expect(telemetryTierSize(4)).toBe(0);
  });
});

describe("dequantiseLog", () => {
  it("reserves 0 for zero-or-below-floor", () => {
    expect(dequantiseLog(0)).toBe(0);
  });

  it("places 0 dBFS at q=200", () => {
    expect(dequantiseLog(200)).toBeCloseTo(1.0, 6);
  });

  it("steps 0.5 dB per count", () => {
    const a = dequantiseLog(100);
    const b = dequantiseLog(101);
    expect(20 * Math.log10(b / a)).toBeCloseTo(0.5, 6);
  });

  it("round-trips every quantiser code within half a step", () => {
    // The guarantee the meters rely on: ~6% relative error, uniformly, across the range.
    for (let code = 1; code <= 255; code++) {
      const v = dequantiseLog(code);
      expect(q(v)).toBe(code);
    }
  });

  it("round-trips real magnitudes within 0.5 dB", () => {
    // Values spanning what this device actually sees: the measured room noise floor
    // (0.0006), normal music RMS, and the largest band-0 flux observed (~3.5).
    for (const v of [6e-4, 0.001, 0.02, 0.35, 1.0, 3.5, 12]) {
      const back = dequantiseLog(q(v));
      expect(Math.abs(20 * Math.log10(back / v))).toBeLessThanOrEqual(0.25);
    }
  });

  it("collapses anything below the representable floor to zero", () => {
    // q=1 is -99.5 dBFS = 1.06e-5, so 1e-5 is genuinely below the floor and reads as zero
    // rather than as a tiny value. That is ~35 dB below this device's measured room noise,
    // so it only ever happens for true silence.
    expect(q(1e-5)).toBe(0);
    expect(dequantiseLog(q(1e-5))).toBe(0);
    expect(dequantiseLog(1)).toBeCloseTo(Math.pow(10, -99.5 / 20), 12);
  });

  it("saturates at the top of the ladder", () => {
    // +27.5 dB, ~17 dB above the largest flux ever measured on music.
    expect(q(1e6)).toBe(255);
    expect(dequantiseLog(255)).toBeCloseTo(Math.pow(10, 27.5 / 20), 6);
  });
});

describe("magnitudeToDb", () => {
  it("floors at the meter floor rather than -Infinity", () => {
    expect(magnitudeToDb(0)).toBe(TELEMETRY_DB_FLOOR);
    expect(magnitudeToDb(-1)).toBe(TELEMETRY_DB_FLOOR);
    expect(magnitudeToDb(1e-30)).toBe(TELEMETRY_DB_FLOOR);
  });

  it("maps full scale to 0 dBFS", () => {
    expect(magnitudeToDb(1)).toBeCloseTo(0, 6);
    expect(magnitudeToDb(0.1)).toBeCloseTo(-20, 6);
  });
});

describe("decodeTelemetryFrame", () => {
  it("decodes a meters frame field by field", () => {
    const bytes = makeFrame({
      tier: 1,
      seq: 0x1234,
      dropped: 7,
      gainSteps: 13,
      rmsInput: 0.02,
      rmsInstant: 0.03,
      peak: 0.5,
      noiseFloor: 0.0006,
      clipCount: 9,
      framesSinceStep: 55,
      flux: [0.4, 0.2, 0.1, 0.05],
      threshold: [0.3, 0.25, 0.2, 0.15],
    });
    const f = decodeTelemetryFrame(bytes)!;
    expect(f).not.toBeNull();
    expect(f.tier).toBe(1);
    expect(f.seq).toBe(0x1234);
    expect(f.dropped).toBe(7);
    expect(f.gainSteps).toBe(13);
    expect(f.gainDb).toBeCloseTo(6.5, 6);
    expect(f.clipCount).toBe(9);
    expect(f.framesSinceStep).toBe(55);
    expect(f.rmsInput).toBeCloseTo(0.02, 3);
    expect(f.peak).toBeCloseTo(0.5, 2);
    expect(f.flux[0]).toBeCloseTo(0.4, 2);
    expect(f.threshold[3]).toBeCloseTo(0.15, 2);
    // Tier 1 carries no raw stats; they must read as absent, not as zero-valued data.
    expect(f.mean).toBeNull();
    expect(f.sigma).toBeNull();
    expect(f.buckets).toBeNull();
  });

  it("decodes a negative gain (int8 through a uint8 byte)", () => {
    // The firmware writes a signed step count through a (uint8_t) cast. Reading it back
    // unsigned would turn -20 dB of gain into +107.5 dB on the meter.
    const f = decodeTelemetryFrame(makeFrame({ gainSteps: -40 }))!;
    expect(f.gainSteps).toBe(-40);
    expect(f.gainDb).toBeCloseTo(-20, 6);
  });

  it("decodes the full signed gain range", () => {
    for (const steps of [-40, -1, 0, 1, 40]) {
      expect(
        decodeTelemetryFrame(makeFrame({ gainSteps: steps }))!.gainSteps,
      ).toBe(steps);
    }
  });

  it("decodes every flag independently", () => {
    expect(decodeTelemetryFrame(makeFrame({ silent: true }))!.silent).toBe(
      true,
    );
    expect(decodeTelemetryFrame(makeFrame({ clipped: true }))!.clipped).toBe(
      true,
    );
    expect(
      decodeTelemetryFrame(makeFrame({ agcFrozen: true }))!.agcFrozen,
    ).toBe(true);
    expect(
      decodeTelemetryFrame(makeFrame({ thresholdMode: 1 }))!.thresholdMode,
    ).toBe(1);
    const none = decodeTelemetryFrame(makeFrame({}))!;
    expect(none.silent).toBe(false);
    expect(none.clipped).toBe(false);
    expect(none.agcFrozen).toBe(false);
    expect(none.thresholdMode).toBe(0);
  });

  it("maps each beat bit to its own band", () => {
    // A transposition here would light the wrong band chip and send someone tuning the
    // wrong parameter, which is worse than showing nothing.
    for (let b = 0; b < AUDIO_NUM_BANDS; b++) {
      const f = decodeTelemetryFrame(makeFrame({ beatMask: 1 << b }))!;
      expect(f.beats[b]).toBe(true);
      expect(f.beats.filter(Boolean)).toHaveLength(1);
      expect(f.beatMask).toBe(1 << b);
    }
  });

  it("does not let beat bits bleed into the flag bits", () => {
    const f = decodeTelemetryFrame(makeFrame({ beatMask: 0x0f }))!;
    expect(f.beats).toEqual([true, true, true, true]);
    expect(f.silent).toBe(false);
    expect(f.clipped).toBe(false);
    expect(f.agcFrozen).toBe(false);
    expect(f.thresholdMode).toBe(0);
  });

  it("reads a little-endian seq", () => {
    expect(decodeTelemetryFrame(makeFrame({ seq: 0x0102 }))!.seq).toBe(0x0102);
    expect(decodeTelemetryFrame(makeFrame({ seq: 0xffff }))!.seq).toBe(0xffff);
  });

  it("adds raw stats at tier 2 and buckets at tier 3", () => {
    const stats = decodeTelemetryFrame(
      makeFrame({
        tier: 2,
        mean: [0.1, 0.2, 0.3, 0.4],
        sigma: [0.01, 0.02, 0.03, 0.04],
      }),
    )!;
    expect(stats.tier).toBe(2);
    expect(stats.mean![1]).toBeCloseTo(0.2, 2);
    expect(stats.sigma![3]).toBeCloseTo(0.04, 3);
    expect(stats.buckets).toBeNull();

    const spec = decodeTelemetryFrame(
      makeFrame({
        tier: 3,
        buckets: Array.from({ length: 20 }, (_, i) => 0.01 * (i + 1)),
      }),
    )!;
    expect(spec.tier).toBe(3);
    expect(spec.buckets).toHaveLength(20);
    expect(spec.buckets![0]).toBeCloseTo(0.01, 3);
    expect(spec.buckets![19]).toBeCloseTo(0.2, 2);
  });

  it("decodes each tier as a byte-exact prefix of the next", () => {
    // The nesting property the format is built on. A tier-3 frame truncated to 20 bytes must
    // decode identically to the tier-1 frame carrying the same values.
    const common = {
      seq: 999,
      gainSteps: -7,
      rmsInput: 0.05,
      peak: 0.6,
      flux: [0.4, 0.3, 0.2, 0.1],
      threshold: [0.35, 0.25, 0.15, 0.05],
    };
    const three = makeFrame({ ...common, tier: 3, mean: [1, 1, 1, 1] });
    const truncated = three.slice(0, TIER_SIZE_METERS);
    truncated[0] = (1 << 4) | 1; // relabel the header as tier 1
    const a = decodeTelemetryFrame(truncated)!;
    const b = decodeTelemetryFrame(makeFrame({ ...common, tier: 1 }))!;
    expect(a).toEqual(b);
  });

  it("discards a frame with an unknown version rather than guessing", () => {
    expect(decodeTelemetryFrame(makeFrame({ version: 2 }))).toBeNull();
    expect(decodeTelemetryFrame(makeFrame({ version: 0 }))).toBeNull();
  });

  it("discards a tier the header does not name", () => {
    expect(decodeTelemetryFrame(makeFrame({ tier: 0 }))).toBeNull();
    const bogus = makeFrame({ tier: 1 });
    bogus[0] = (1 << 4) | 7;
    expect(decodeTelemetryFrame(bogus)).toBeNull();
  });

  it("discards a truncated frame", () => {
    const full = makeFrame({ tier: 3 });
    expect(decodeTelemetryFrame(full.slice(0, 47))).toBeNull();
    expect(
      decodeTelemetryFrame(makeFrame({ tier: 1 }).slice(0, 19)),
    ).toBeNull();
    expect(decodeTelemetryFrame(new Uint8Array(0))).toBeNull();
    expect(decodeTelemetryFrame(null)).toBeNull();
    expect(decodeTelemetryFrame(undefined)).toBeNull();
  });

  it("ignores trailing bytes past the tier size", () => {
    // Forward compatibility: a future firmware adding tier 4 must not blind this build.
    const f = decodeTelemetryFrame(
      makeFrame({ tier: 1, seq: 42, trailing: [9, 9, 9, 9] }),
    )!;
    expect(f).not.toBeNull();
    expect(f.seq).toBe(42);
    expect(f.tier).toBe(1);
  });

  it("decodes from base64 and tolerates junk", () => {
    const bytes = makeFrame({ seq: 77, gainSteps: 5 });
    const f = decodeTelemetryFrameFromBase64(toBase64(bytes))!;
    expect(f.seq).toBe(77);
    expect(f.gainSteps).toBe(5);
    expect(decodeTelemetryFrameFromBase64(null)).toBeNull();
    expect(decodeTelemetryFrameFromBase64("")).toBeNull();
  });
});

describe("ring buffer", () => {
  it("starts empty", () => {
    const ring = createTelemetryRing(4);
    expect(ring.count).toBe(0);
    expect(ringIndex(ring, 0)).toBe(-1);
  });

  it("stores and reads back the newest frame", () => {
    const ring = createTelemetryRing(4);
    expect(
      pushTelemetryBytes(ring, makeFrame({ seq: 5, gainSteps: -3 }), 100),
    ).toBe(true);
    const i = ringIndex(ring, 0);
    expect(ring.seq[i]).toBe(5);
    expect(ring.gain[i]).toBe(-3);
    expect(ring.timeMs[i]).toBe(100);
  });

  it("wraps, keeping the most recent `capacity` frames", () => {
    const ring = createTelemetryRing(4);
    for (let n = 0; n < 10; n++) {
      pushTelemetryBytes(ring, makeFrame({ seq: n }), n * 10);
    }
    expect(ring.count).toBe(10);
    expect(ring.seq[ringIndex(ring, 0)]).toBe(9);
    expect(ring.seq[ringIndex(ring, 3)]).toBe(6);
    expect(ringIndex(ring, 4)).toBe(-1);
  });

  it("rejects a bad frame without disturbing the ring", () => {
    // A corrupt notification must not poison the history the wizard later replays.
    const ring = createTelemetryRing(4);
    pushTelemetryBytes(ring, makeFrame({ seq: 1 }), 0);
    expect(pushTelemetryBytes(ring, makeFrame({ version: 9 }), 10)).toBe(false);
    expect(pushTelemetryBytes(ring, new Uint8Array([0x11]), 20)).toBe(false);
    expect(ring.count).toBe(1);
    expect(ring.seq[ringIndex(ring, 0)]).toBe(1);
  });

  it("zeroes raw stats for a tier-1 frame instead of leaving the slot stale", () => {
    // A window spanning a tier change must not mix this frame's flux with an older
    // frame's mean — that would silently corrupt an offline sensitivity fit.
    const ring = createTelemetryRing(2);
    pushTelemetryBytes(
      ring,
      makeFrame({ tier: 2, mean: [1, 1, 1, 1], sigma: [1, 1, 1, 1] }),
      0,
    );
    pushTelemetryBytes(
      ring,
      makeFrame({ tier: 2, mean: [1, 1, 1, 1], sigma: [1, 1, 1, 1] }),
      10,
    );
    pushTelemetryBytes(ring, makeFrame({ tier: 1 }), 20); // reuses slot 0
    const i = ringIndex(ring, 0);
    for (let b = 0; b < AUDIO_NUM_BANDS; b++) {
      expect(ring.mean[i * AUDIO_NUM_BANDS + b]).toBe(0);
      expect(ring.sigma[i * AUDIO_NUM_BANDS + b]).toBe(0);
    }
  });

  it("preserves quantised bytes exactly", () => {
    const ring = createTelemetryRing(2);
    pushTelemetryBytes(ring, makeFrame({ flux: [0.4, 0.2, 0.1, 0.05] }), 0);
    const i = ringIndex(ring, 0);
    expect(ring.flux[i * AUDIO_NUM_BANDS]).toBe(q(0.4));
    expect(ring.flux[i * AUDIO_NUM_BANDS + 3]).toBe(q(0.05));
  });
});

describe("summarizeTelemetry", () => {
  function fill(frames: { bytes: Uint8Array; timeMs: number }[]) {
    const ring = createTelemetryRing(512);
    for (const f of frames) pushTelemetryBytes(ring, f.bytes, f.timeMs);
    return ring;
  }

  it("returns the empty summary with no frames", () => {
    expect(summarizeTelemetry(createTelemetryRing(8), 1000)).toEqual(
      EMPTY_SUMMARY,
    );
  });

  it("reports live within the stale window and frozen past it", () => {
    const ring = fill([{ bytes: makeFrame({}), timeMs: 1000 }]);
    expect(summarizeTelemetry(ring, 1500).live).toBe(true);
    expect(summarizeTelemetry(ring, 2500).live).toBe(false);
    expect(summarizeTelemetry(ring, 2500).ageMs).toBe(1500);
  });

  it("counts beats per second over the window", () => {
    // 120 BPM = 2 beats/s, sampled at 8 Hz so every beat lands in its own frame.
    const ring = fill(makeStream({ bpm: 120, seconds: 10, rateHz: 8 }));
    const s = summarizeTelemetry(ring, 10_000);
    expect(s.beatsPerSecond).toBeGreaterThan(1.7);
    expect(s.beatsPerSecond).toBeLessThan(2.3);
  });

  it("estimates BPM from the median inter-beat interval", () => {
    for (const bpm of [90, 120, 140]) {
      const ring = fill(makeStream({ bpm, seconds: 10, rateHz: 16 }));
      const s = summarizeTelemetry(ring, 10_000);
      expect(s.bpm).not.toBeNull();
      // Sticky-OR'd beat bits are quantised to the send interval, so tolerate that much.
      expect(Math.abs(s.bpm! - bpm)).toBeLessThanOrEqual(12);
    }
  });

  it("refuses a BPM when there are too few beats", () => {
    const ring = fill(makeStream({ bpm: 120, seconds: 1, rateHz: 8 }));
    expect(summarizeTelemetry(ring, 1000).bpm).toBeNull();
  });

  it("refuses an implausible BPM rather than clamping it", () => {
    // A mis-tuned threshold firing on every frame would otherwise render as a confident
    // "480 BPM" on the screen whose whole purpose is fixing that tuning.
    const frames = Array.from({ length: 40 }, (_, n) => ({
      bytes: makeFrame({ beatMask: 0x1 }),
      timeMs: n * 125,
    }));
    expect(summarizeTelemetry(fill(frames), 5000).bpm).toBeNull();
  });

  it("reports the lowest band that fired on the newest beat", () => {
    const ring = fill([{ bytes: makeFrame({ beatMask: 0b1010 }), timeMs: 0 }]);
    expect(summarizeTelemetry(ring, 100).lastBeatBand).toBe(1);
  });

  it("measures the silent fraction", () => {
    const frames = Array.from({ length: 10 }, (_, n) => ({
      bytes: makeFrame({ silent: n < 4 }),
      timeMs: n * 100,
    }));
    expect(summarizeTelemetry(fill(frames), 1000).silentFraction).toBeCloseTo(
      0.4,
      6,
    );
  });

  it("measures the clip fraction", () => {
    const ring = fill(makeStream({ seconds: 4, rateHz: 8, clipEvery: 4 }));
    expect(summarizeTelemetry(ring, 4000).clipFraction).toBeCloseTo(0.25, 2);
  });

  it("excludes frames outside the window", () => {
    const old = Array.from({ length: 10 }, (_, n) => ({
      bytes: makeFrame({ silent: true }),
      timeMs: n * 100,
    }));
    const recent = Array.from({ length: 10 }, (_, n) => ({
      bytes: makeFrame({ silent: false }),
      timeMs: 20_000 + n * 100,
    }));
    const s = summarizeTelemetry(fill([...old, ...recent]), 21_000, 10_000);
    expect(s.frames).toBe(10);
    expect(s.silentFraction).toBe(0);
  });

  it("reports gain, and flags it pinned at the AGC ceiling", () => {
    const ring = fill([
      { bytes: makeFrame({ gainSteps: GAIN_STEPS_MAX }), timeMs: 0 },
    ]);
    const s = summarizeTelemetry(ring, 100);
    expect(s.gainDb).toBeCloseTo(20, 6);
    expect(s.gainPinnedHigh).toBe(true);
    expect(s.gainPinnedLow).toBe(false);
  });

  it("counts gain changes in the window", () => {
    const frames = [0, 0, 1, 1, 2, 2, 2, 3].map((g, n) => ({
      bytes: makeFrame({ gainSteps: g }),
      timeMs: n * 100,
    }));
    expect(summarizeTelemetry(fill(frames), 800).gainChanges).toBe(3);
  });

  it("derives headroom from the loudest recent peak", () => {
    const ring = fill([{ bytes: makeFrame({ peak: 0.25 }), timeMs: 0 }]);
    const s = summarizeTelemetry(ring, 100);
    expect(s.peakDb).toBeCloseTo(-12, 0);
    expect(s.headroomDb).toBeCloseTo(12, 0);
  });

  it("takes the maximum peak across the window, not the newest", () => {
    const frames = [0.5, 0.9, 0.1].map((p, n) => ({
      bytes: makeFrame({ peak: p }),
      timeMs: n * 100,
    }));
    const s = summarizeTelemetry(fill(frames), 300);
    expect(s.peakDb).toBeCloseTo(magnitudeToDb(0.9), 0);
  });

  it("accumulates dropped ticks across a wrapping counter", () => {
    // Differencing the endpoints would read 0 across a full wrap and report a clean stream.
    const frames = [250, 254, 2, 6].map((d, n) => ({
      bytes: makeFrame({ dropped: d }),
      timeMs: n * 100,
    }));
    expect(summarizeTelemetry(fill(frames), 400).droppedInWindow).toBe(12);
  });

  it("carries the threshold mode and frozen flag from the newest frame", () => {
    const ring = fill([
      { bytes: makeFrame({ thresholdMode: 0 }), timeMs: 0 },
      { bytes: makeFrame({ thresholdMode: 1, agcFrozen: true }), timeMs: 100 },
    ]);
    const s = summarizeTelemetry(ring, 200);
    expect(s.thresholdMode).toBe(1);
    expect(s.agcFrozen).toBe(true);
  });

  it("reports the tier actually being received", () => {
    const ring = fill([{ bytes: makeFrame({ tier: 3 }), timeMs: 0 }]);
    expect(summarizeTelemetry(ring, 100).tier).toBe(TELEMETRY_TIER_SPECTRUM);
  });
});
