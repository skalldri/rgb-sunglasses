/**
 * TypeScript mirror of the firmware's FFT Bars display mapping
 * (fw/src/animations/fft_bar_mapping.h): bucket power → bar-height fraction in [0, 1].
 *
 *   powerDb = 10·log10(max(power, POWER_FLOOR))
 *   height  = clamp((powerDb + tilt·octaves[bucket] + gain(energyScale) − floorDb) / rangeDb, 0, 1)
 *
 * Pinned to the firmware by app/__tests__/fixtures/fft-bar-vectors.json, which
 * fw/tools/gen_fft_bar_vectors.cpp emits from the C++ header. This is what a "match the
 * glasses" spectrum view would draw; the monitor panel's spectrum stays peak-normalised
 * today, so this module is currently only the tested mirror.
 */

import { AUDIO_NUM_DISPLAY_BUCKETS } from "@/services/audio-telemetry";

export interface FftBarWindow {
    /** dB of bucket power at which a bar starts to light. */
    floorDb: number;
    /** dB from an empty bar to a full one. */
    rangeDb: number;
    /** Pink-noise compensation, dB per octave above bucket 0. */
    tiltDbPerOctave: number;
    /** Legacy `audio/fft_energy_scale`: relative gain, 0 dB at ENERGY_SCALE_UNITY. */
    energyScale: number;
}

/** The `audio/fft_energy_scale` value that is 0 dB of gain (its firmware default). */
export const FFT_BAR_ENERGY_SCALE_UNITY = 20;

/** Power below this reads as −90 dB, keeping log10 finite for a silent bucket. */
export const FFT_BAR_POWER_FLOOR = 1e-9;

/**
 * log2(centre_b / centre_0) per display bucket; bucket 0 (bins 2–5, 109 Hz) is the
 * reference. Transcribed from the firmware header; the fixture test checks it.
 */
export const FFT_BAR_BUCKET_OCTAVES: readonly number[] = [
    0.0, 1.0995, 1.585, 1.8931, 2.1468, 2.3626, 2.6521, 2.9206, 3.1234, 3.3424,
    3.5677, 3.7318, 3.8791, 4.0, 4.0753, 4.1584, 4.2907, 4.45, 4.5935, 4.724,
];

/** Bucket power → dB, floored so 0, negative and NaN all read as −90 dB. */
export function fftBarPowerDb(power: number): number {
    const p = power > FFT_BAR_POWER_FLOOR ? power : FFT_BAR_POWER_FLOOR;
    return 10 * Math.log10(p);
}

/** Legacy energy-scale parameter → relative gain in dB (0 at the unity value). */
export function fftBarGainDb(energyScale: number): number {
    if (!(energyScale > 0)) return 0;
    return 10 * Math.log10(energyScale / FFT_BAR_ENERGY_SCALE_UNITY);
}

/** Bar-height fraction in [0, 1] for one bucket's power under `w`. */
export function fftBarHeight(power: number, bucket: number, w: FftBarWindow): number {
    // No signal → no bar, whatever the window (mirrors the firmware's early-out).
    if (!(power > 0)) return 0;
    const octaves =
        bucket >= 0 && bucket < AUDIO_NUM_DISPLAY_BUCKETS ? FFT_BAR_BUCKET_OCTAVES[bucket] : 0;
    const range = w.rangeDb > 0 ? w.rangeDb : 1;
    const h =
        (fftBarPowerDb(power) + w.tiltDbPerOctave * octaves + fftBarGainDb(w.energyScale) - w.floorDb) /
        range;
    if (!(h > 0)) return 0;
    return h > 1 ? 1 : h;
}

/** Whole rows lit on a panel of `rows` rows, rounded the way the firmware does. */
export function fftBarRows(fraction: number, rows: number): number {
    return Math.floor(fraction * rows + 0.5);
}
