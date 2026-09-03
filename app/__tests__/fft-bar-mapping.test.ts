/**
 * Pins app/services/fft-bar-mapping.ts to numbers the FIRMWARE computed. Regenerate the
 * fixture after any change to fw/src/animations/fft_bar_mapping.h or the FFT display
 * defaults in fw/src/sound/audio_param_table.h (from the repo root):
 *
 *   g++ -std=c++2b -I fw/src -I fw/src/sound -I fw/include -o /tmp/gen_fft_bars fw/tools/gen_fft_bar_vectors.cpp -lm
 *   /tmp/gen_fft_bars > app/__tests__/fixtures/fft-bar-vectors.json
 */

import vector from "./fixtures/fft-bar-vectors.json";
import { AUDIO_PARAMS } from "@/services/audio-params";
import {
    FFT_BAR_BUCKET_OCTAVES,
    FFT_BAR_ENERGY_SCALE_UNITY,
    FFT_BAR_POWER_FLOOR,
    fftBarGainDb,
    fftBarHeight,
    fftBarPowerDb,
    fftBarRows,
    type FftBarWindow,
} from "@/services/fft-bar-mapping";

type Vector = {
    defaults: {
        floorDb: number;
        rangeDb: number;
        tiltDbPerOctave: number;
        energyScale: number;
        smoothingCoeff: number;
        energyScaleUnity: number;
        powerFloor: number;
    };
    bucketOctaves: number[];
    vectors: {
        name: string;
        power: number;
        bucket: number;
        floorDb: number;
        rangeDb: number;
        tiltDbPerOctave: number;
        energyScale: number;
        powerDb: number;
        fraction: number;
        rows12: number;
    }[];
};

const V = vector as Vector;

describe("fft-bar-mapping — against firmware-generated vectors", () => {
    it("shares the firmware's constants", () => {
        expect(FFT_BAR_ENERGY_SCALE_UNITY).toBe(V.defaults.energyScaleUnity);
        expect(FFT_BAR_POWER_FLOOR).toBeCloseTo(V.defaults.powerFloor, 15);
        expect(FFT_BAR_BUCKET_OCTAVES).toHaveLength(V.bucketOctaves.length);
        V.bucketOctaves.forEach((oct, b) => {
            expect(FFT_BAR_BUCKET_OCTAVES[b]).toBeCloseTo(oct, 4);
        });
    });

    it("uses the same display defaults the tuning screen advertises", () => {
        expect(AUDIO_PARAMS.fftFloorDb.defaultValue).toBe(V.defaults.floorDb);
        expect(AUDIO_PARAMS.fftRangeDb.defaultValue).toBe(V.defaults.rangeDb);
        expect(AUDIO_PARAMS.fftTiltDbOct.defaultValue).toBe(V.defaults.tiltDbPerOctave);
        expect(AUDIO_PARAMS.fftEnergyScale.defaultValue).toBe(V.defaults.energyScale);
        expect(AUDIO_PARAMS.fftSmoothingCoeff.defaultValue).toBeCloseTo(V.defaults.smoothingCoeff, 6);
    });

    it("reproduces every firmware vector", () => {
        expect(V.vectors.length).toBeGreaterThan(10);
        for (const v of V.vectors) {
            const w: FftBarWindow = {
                floorDb: v.floorDb,
                rangeDb: v.rangeDb,
                tiltDbPerOctave: v.tiltDbPerOctave,
                energyScale: v.energyScale,
            };
            // float32 log10 on the device vs double here: agree to ~1e-6, well inside a row.
            expect([v.name, fftBarPowerDb(v.power)]).toEqual([v.name, expect.closeTo(v.powerDb, 4)]);
            expect([v.name, fftBarHeight(v.power, v.bucket, w)]).toEqual([
                v.name,
                expect.closeTo(v.fraction, 5),
            ]);
            expect([v.name, fftBarRows(fftBarHeight(v.power, v.bucket, w), 12)]).toEqual([
                v.name,
                v.rows12,
            ]);
        }
    });
});

describe("fft-bar-mapping — edge behaviour", () => {
    const d: FftBarWindow = { floorDb: -36, rangeDb: 36, tiltDbPerOctave: 3, energyScale: 20 };

    it("is dark and finite for degenerate power", () => {
        for (const p of [0, -1, NaN, -Infinity]) {
            expect(Number.isFinite(fftBarPowerDb(p))).toBe(true);
            expect(fftBarHeight(p, 0, d)).toBe(0);
        }
    });

    it("clamps to [0, 1] and is monotonic in power", () => {
        let prev = -1;
        for (let e = 1e-9; e <= 1e4; e *= 1.5) {
            const h = fftBarHeight(e, 0, d);
            expect(h).toBeGreaterThanOrEqual(prev);
            expect(h).toBeGreaterThanOrEqual(0);
            expect(h).toBeLessThanOrEqual(1);
            prev = h;
        }
    });

    it("treats the energy scale as a relative gain around its unity value", () => {
        expect(fftBarGainDb(FFT_BAR_ENERGY_SCALE_UNITY)).toBeCloseTo(0, 9);
        expect(fftBarGainDb(200)).toBeCloseTo(10, 9);
        expect(fftBarGainDb(2)).toBeCloseTo(-10, 9);
        expect(fftBarGainDb(0)).toBe(0);
        expect(fftBarGainDb(NaN)).toBe(0);
    });

    it("gives out-of-range buckets no tilt", () => {
        expect(fftBarHeight(0.1, 20, d)).toBeCloseTo(fftBarHeight(0.1, 0, d), 9);
        expect(fftBarHeight(0.1, -1, d)).toBeCloseTo(fftBarHeight(0.1, 0, d), 9);
    });
});
