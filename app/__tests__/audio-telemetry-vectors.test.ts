import {
  createTelemetryRing,
  decodeTelemetryFrame,
  dequantiseLog,
  pushTelemetryBytes,
  ringIndex,
} from "@/services/audio-telemetry";
import vectors from "./fixtures/audio-telemetry-vectors.json";

/**
 * Cross-check the app decoder against the ACTUAL FIRMWARE PACKER.
 *
 * Every other test in this suite checks the decoder against a fixture I wrote from the spec,
 * which proves the decoder matches my reading of the format — not that it matches the bytes
 * the device will really send. These vectors are different: they were produced by compiling
 * fw/src/sound/audio_telemetry_codec.h on the host and calling audio_telemetry_pack()
 * directly, so a disagreement here means the app and the firmware genuinely disagree.
 *
 * Regenerate after any wire change (from the repo root):
 *
 *   g++ -std=c++2b -I fw/src -I fw/src/sound -o /tmp/gen_vectors fw/tools/gen_telemetry_vectors.cpp -lm
 *   /tmp/gen_vectors > app/__tests__/fixtures/audio-telemetry-vectors.json
 *
 * This is not a substitute for hardware — it proves the codec agrees, not that the notify
 * path, the MTU clamp or the subscription lifecycle work. It does mean that when hardware is
 * available, a wrong number on screen is a transport or wiring bug, not a decode bug.
 */

type Vector = {
  name: string;
  tier: number;
  bytes: number[];
  expect: {
    seq: number;
    dropped: number;
    gainSteps: number;
    clipCount: number;
    framesSinceStep: number;
    silent: boolean;
    clipped: boolean;
    agcFrozen: boolean;
    thresholdMode: number;
    beatMask: number;
    rmsInput: number;
    rmsInstant: number;
    peak: number;
    noiseFloor: number;
    flux: number[];
    threshold: number[];
  };
};

const VECTORS = vectors as Vector[];

describe("firmware-generated wire vectors", () => {
  it("has vectors covering every tier", () => {
    expect(VECTORS.length).toBeGreaterThanOrEqual(5);
    expect(new Set(VECTORS.map((v) => v.tier))).toEqual(new Set([1, 2, 3]));
  });

  it.each(VECTORS.map((v) => [`${v.name} (tier ${v.tier})`, v] as const))(
    "decodes %s exactly as the firmware packed it",
    (_name, v) => {
      const frame = decodeTelemetryFrame(new Uint8Array(v.bytes));
      expect(frame).not.toBeNull();
      const f = frame!;

      expect(f.tier).toBe(v.tier);
      expect(v.bytes).toHaveLength({ 1: 20, 2: 28, 3: 48 }[v.tier]!);

      // Lossless fields must match bit for bit.
      expect(f.seq).toBe(v.expect.seq);
      expect(f.dropped).toBe(v.expect.dropped);
      expect(f.gainSteps).toBe(v.expect.gainSteps);
      expect(f.clipCount).toBe(v.expect.clipCount);
      expect(f.framesSinceStep).toBe(v.expect.framesSinceStep);
      expect(f.silent).toBe(v.expect.silent);
      expect(f.clipped).toBe(v.expect.clipped);
      expect(f.agcFrozen).toBe(v.expect.agcFrozen);
      expect(f.thresholdMode).toBe(v.expect.thresholdMode);
      expect(f.beatMask).toBe(v.expect.beatMask);

      // Quantised magnitudes: the expectations are the firmware's OWN dq_log() output, so
      // any gap is this decoder disagreeing with the device's inverse, not quantiser loss.
      // The firmware iterates a multiplication ladder where this uses Math.pow, so allow
      // the ladder's accumulated rounding (~3e-5 relative) and nothing more.
      const close = (got: number, want: number, what: string) => {
        if (want === 0) {
          expect(got).toBe(0);
          return;
        }
        const rel = Math.abs(got - want) / want;
        expect(`${what}: ${rel}`).toBe(`${what}: ${rel}`);
        expect(rel).toBeLessThan(1e-4);
      };
      close(f.rmsInput, v.expect.rmsInput, "rmsInput");
      close(f.rmsInstant, v.expect.rmsInstant, "rmsInstant");
      close(f.peak, v.expect.peak, "peak");
      close(f.noiseFloor, v.expect.noiseFloor, "noiseFloor");
      for (let b = 0; b < 4; b++) {
        close(f.flux[b], v.expect.flux[b], `flux[${b}]`);
        close(f.threshold[b], v.expect.threshold[b], `threshold[${b}]`);
      }
    },
  );

  it("reads raw stats only at tier 2+, and buckets only at tier 3", () => {
    for (const v of VECTORS) {
      const f = decodeTelemetryFrame(new Uint8Array(v.bytes))!;
      expect(f.mean === null).toBe(v.tier < 2);
      expect(f.buckets === null).toBe(v.tier < 3);
      if (f.buckets) expect(f.buckets).toHaveLength(20);
    }
  });

  it("agrees that each tier is a byte-exact prefix of the next", () => {
    // The firmware packed the same frame at all three tiers; the shared bytes must be equal.
    const byName = new Map(VECTORS.map((v) => [`${v.name}`, v]));
    const t1 = byName.get("typical_meters")!;
    const t2 = byName.get("typical_stats")!;
    const t3 = byName.get("typical_spectrum")!;
    expect(t2.bytes.slice(1, 20)).toEqual(t1.bytes.slice(1, 20));
    expect(t3.bytes.slice(1, 28)).toEqual(t2.bytes.slice(1, 28));
    // Byte 0 differs only in the tier nibble.
    expect(t1.bytes[0] >> 4).toBe(t2.bytes[0] >> 4);
    expect(t1.bytes[0] & 0x0f).toBe(1);
    expect(t2.bytes[0] & 0x0f).toBe(2);
    expect(t3.bytes[0] & 0x0f).toBe(3);
  });
});

describe("firmware vectors through the PRODUCTION decoder", () => {
  /**
   * The tests above exercise decodeTelemetryFrame, which nothing on the notification path
   * calls: the shipping path is pushTelemetryBytes, a separate hand-written parser that
   * decodes straight into the ring so it can allocate nothing per frame. Verifying only the
   * first meant the cross-check this PR advertises — "app decode verified against firmware
   * encode" — covered a test-only function while the real decoder went unchecked.
   *
   * The shared OFF_* constants mean a wrong offset VALUE would still have been caught. What
   * would not: a per-field mapping mistake in pushTelemetryBytes — a swapped pair, a field
   * read with the wrong dequantiser. That is exactly what these assert.
   */
  it.each(VECTORS.map((v) => [`${v.name} (tier ${v.tier})`, v] as const))(
    "pushes %s into the ring with every field intact",
    (_name, v) => {
      const ring = createTelemetryRing(8);
      expect(pushTelemetryBytes(ring, new Uint8Array(v.bytes), 1000)).toBe(
        true,
      );

      const i = ringIndex(ring, 0);
      expect(i).toBeGreaterThanOrEqual(0);

      // Lossless fields, read back out of the ring's own typed arrays.
      expect(ring.seq[i]).toBe(v.expect.seq);
      expect(ring.dropped[i]).toBe(v.expect.dropped);
      expect(ring.gain[i]).toBe(v.expect.gainSteps);
      expect(ring.clips[i]).toBe(v.expect.clipCount);
      expect(ring.sinceStep[i]).toBe(v.expect.framesSinceStep);
      expect(ring.tier[i]).toBe(v.tier);

      // Flags, decomposed the way the consumers read them.
      const flags = ring.flags[i];
      expect((flags & 0x01) !== 0).toBe(v.expect.silent);
      expect((flags & 0x02) !== 0).toBe(v.expect.clipped);
      expect((flags & 0x04) !== 0).toBe(v.expect.agcFrozen);
      expect((flags & 0x08) !== 0 ? 1 : 0).toBe(v.expect.thresholdMode);
      expect((flags >> 4) & 0x0f).toBe(v.expect.beatMask);

      // Quantised magnitudes, dequantised exactly as the provider and panel do.
      const close = (got: number, want: number) => {
        if (want === 0) {
          expect(got).toBe(0);
          return;
        }
        expect(Math.abs(got - want) / want).toBeLessThan(1e-4);
      };
      close(dequantiseLog(ring.rmsIn[i]), v.expect.rmsInput);
      close(dequantiseLog(ring.rmsInst[i]), v.expect.rmsInstant);
      close(dequantiseLog(ring.peak[i]), v.expect.peak);
      close(dequantiseLog(ring.noise[i]), v.expect.noiseFloor);

      const base = i * 4;
      for (let b = 0; b < 4; b++) {
        close(dequantiseLog(ring.flux[base + b]), v.expect.flux[b]);
        close(dequantiseLog(ring.threshold[base + b]), v.expect.threshold[b]);
      }
    },
  );

  it("agrees field-for-field with decodeTelemetryFrame on every vector", () => {
    // The two parsers must not drift apart. This is the assertion that makes keeping both
    // defensible: one is the readable reference, the other is the zero-allocation path.
    for (const v of VECTORS) {
      const bytes = new Uint8Array(v.bytes);
      const ref = decodeTelemetryFrame(bytes)!;
      const ring = createTelemetryRing(4);
      pushTelemetryBytes(ring, bytes, 0);
      const i = ringIndex(ring, 0);

      expect([v.name, ring.seq[i]]).toEqual([v.name, ref.seq]);
      expect([v.name, ring.gain[i]]).toEqual([v.name, ref.gainSteps]);
      expect([v.name, ring.dropped[i]]).toEqual([v.name, ref.dropped]);
      expect([v.name, ring.clips[i]]).toEqual([v.name, ref.clipCount]);
      expect([v.name, ring.tier[i]]).toEqual([v.name, ref.tier]);
      expect([v.name, (ring.flags[i] >> 4) & 0x0f]).toEqual([
        v.name,
        ref.beatMask,
      ]);
      for (let b = 0; b < 4; b++) {
        expect([v.name, b, dequantiseLog(ring.flux[i * 4 + b])]).toEqual([
          v.name,
          b,
          ref.flux[b],
        ]);
        expect([v.name, b, dequantiseLog(ring.threshold[i * 4 + b])]).toEqual([
          v.name,
          b,
          ref.threshold[b],
        ]);
      }
    }
  });
});
