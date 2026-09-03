#pragma once

#include <sound/audio_dsp.h>

#include <cstddef>
#include <math.h>

/* FFT Bars display mapping: bucket power → bar-height fraction in [0, 1].
 *
 * The bucket energies the DSP exports (audio_analysis_result::display_bucket_energy) are
 * mean |X_k|^2 over each bucket's bins from an UNSCALED 512-pt Hann FFT of the ±1-normalised
 * PCM (audio_dsp.cpp — CMSIS applies no 1/N). They span far more than any linear bar can
 * show: measured on proto0 (2026-09-03, TV at moderate volume, AGC at +20 dB) bucket 0's
 * median was 0.039 while buckets 14–19 sat at 3e-5..6e-5 — a ~60 dB tilt across the 20
 * buckets on top of ~30 dB of dynamics within each. The old `clamp(power * 20)` mapping
 * pinned bucket 0 at full height on 46 % of frames and never lit anything above 600 Hz.
 *
 * So the bar is a dB meter with a fixed window:
 *
 *   dB     = 10·log10(max(E, kFftBarPowerFloor))
 *   height = clamp((dB + tilt(bucket) + gain − floorDb) / rangeDb, 0, 1)
 *
 * - floorDb / rangeDb: the window. The defaults (audio_param_table.h) put the ceiling at
 *   0 dB, i.e. E = 1.0 — the level the AGC's attack path is about to turn the mic down at —
 *   so "red at the top" means what it means on a VU meter. 36 dB over the 12-row proto0
 *   panel is 3 dB per row: about one just-noticeable loudness step per row.
 * - tilt: a pink-noise compensation of `tiltDbPerOctave` × the bucket's centre-frequency
 *   distance from bucket 0 in octaves (kFftBarBucketOctaves), so the mid and treble
 *   buckets get usable height instead of sitting 30–45 dB under the bass.
 * - gain: the legacy `audio/fft_energy_scale` parameter reinterpreted as a relative gain,
 *   0 dB at its default (kFftBarEnergyScaleUnity). A persisted value, or an older app's
 *   "Bar height" slider, keeps doing something sensible: larger is taller.
 *
 * The animation smooths this HEIGHT (not the linear power) with its per-frame EMA, so
 * attack and release are symmetric in dB and an over-ceiling spike contributes exactly 1.0
 * — no overshoot memory holding the bar pinned after the spike is gone.
 *
 * Header-only and Zephyr/BT-free (same seam idiom as audio_frame_fold.h): the animation,
 * its native_sim DI suite, the fw/tests/animations/fft_bar_mapping suite and the host
 * vector generator (fw/tools/gen_fft_bar_vectors.cpp, which pins the app's TypeScript
 * mirror) all compile exactly this. Deliberately NOT part of include/rgbx/ — the extension
 * API exports the raw bucket power and leaves the display mapping to the extension.
 */

struct FftBarWindow {
    float floorDb;         /* dB of bucket power at which a bar starts to light */
    float rangeDb;         /* dB from empty to full */
    float tiltDbPerOctave; /* pink compensation, added per octave above bucket 0 */
    float energyScale;     /* legacy `audio/fft_energy_scale`; unity == 0 dB gain */
};

/* The `audio/fft_energy_scale` value that maps to 0 dB of gain. audio_config.cpp
 * static_asserts this equals that parameter's table default, so a virgin board and a board
 * whose persisted value was never touched both get exactly the documented window. */
inline constexpr float kFftBarEnergyScaleUnity = 20.0f;

/* Power below this reads as −90 dB: keeps log10 finite for a silent bucket (E == 0) and
 * for the negative/NaN garbage a broken frame could carry. Far below any window floor. */
inline constexpr float kFftBarPowerFloor = 1e-9f;

/* log2(centre_b / centre_0) for each display bucket, centre = (start+end)/2 bins. Bucket 0
 * (bins 2–5, 109 Hz) is the reference. Hand-transcribed because log2 is not constexpr;
 * fw/tests/animations/fft_bar_mapping recomputes it from audio_display_bucket_start/end and
 * fails if any entry drifts by more than 0.01. */
inline constexpr float kFftBarBucketOctaves[AUDIO_NUM_DISPLAY_BUCKETS] = {
    0.0000f, 1.0995f, 1.5850f, 1.8931f, 2.1468f, 2.3626f, 2.6521f, 2.9206f, 3.1234f, 3.3424f,
    3.5677f, 3.7318f, 3.8791f, 4.0000f, 4.0753f, 4.1584f, 4.2907f, 4.4500f, 4.5935f, 4.7240f};

/** Bucket power → dB, floored at kFftBarPowerFloor (so 0, negative and NaN read as −90). */
inline float fft_bar_power_db(float energy) {
    /* `!(x > floor)` rather than `x <= floor`: NaN fails every comparison, so this one
     * form catches zero, negative AND non-finite input. */
    if (!(energy > kFftBarPowerFloor)) {
        energy = kFftBarPowerFloor;
    }
    return 10.0f * log10f(energy);
}

/** Legacy energy-scale parameter → relative gain in dB (0 at kFftBarEnergyScaleUnity). */
inline float fft_bar_gain_db(float energyScale) {
    /* The table clamp (0.1..1000) already excludes these; the guard is for a caller that
     * bypassed it, e.g. a test or a default-constructed window. */
    if (!(energyScale > 0.0f)) {
        return 0.0f;
    }
    return 10.0f * log10f(energyScale / kFftBarEnergyScaleUnity);
}

/** Bar-height fraction in [0, 1] for one bucket's power under `w`. */
inline float fft_bar_height(float energy, size_t bucket, const FftBarWindow &w) {
    /* No signal → no bar, whatever the window. Without this, the −90 dB floor plus the
     * maximum tilt (+57 dB on bucket 19) plus +17 dB of legacy gain would clear a low floor
     * and draw a bar out of a bucket that carries literally nothing. Same `!(x > 0)` form as
     * above so NaN is caught too. */
    if (!(energy > 0.0f)) {
        return 0.0f;
    }
    const float tilt =
        (bucket < AUDIO_NUM_DISPLAY_BUCKETS) ? w.tiltDbPerOctave * kFftBarBucketOctaves[bucket]
                                             : 0.0f;
    /* rangeDb's table minimum is 6; a zero here would only come from a bypassed clamp. */
    const float range = (w.rangeDb > 0.0f) ? w.rangeDb : 1.0f;
    const float h = (fft_bar_power_db(energy) + tilt + fft_bar_gain_db(w.energyScale) - w.floorDb) /
                    range;
    if (!(h > 0.0f)) { /* also NaN, should any input be non-finite */
        return 0.0f;
    }
    if (h > 1.0f) {
        return 1.0f;
    }
    return h;
}
