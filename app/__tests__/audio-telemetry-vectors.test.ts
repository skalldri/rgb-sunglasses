import { decodeTelemetryFrame } from "@/services/audio-telemetry";
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
