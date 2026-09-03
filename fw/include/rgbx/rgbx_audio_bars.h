/**
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Stuart Alldritt
 *
 * @file rgbx_audio_bars.h
 * @brief Turn `rgbx_inputs.audio_display_bucket[]` into bar heights the same
 * way the built-in FFT Bars animation does — a dB meter, not a linear clamp.
 *
 * The 20 display buckets are raw power: the mean |X_k|^2 over each bucket's
 * FFT bins from an unscaled 512-point Hann FFT of the ±1-normalised PCM (see
 * `audio_display_bucket` in rgbx_api.h). They are NOT normalised to 0..1. On
 * real audio the bass bucket sits near 0.04–1.0 while the treble buckets sit
 * near 1e-5 — a ~60 dB tilt, on top of ~30 dB of dynamics within a bucket.
 * Drawing `bucket * 255` therefore pins the bass bar and never lights anything
 * above 600 Hz; that was the firmware's own bug until 2026-09.
 *
 * This header is the fix, header-only and import-free apart from `logf`
 * (which is on the SDK allow-list, see rgbx_sys.h):
 *
 *   dB     = 10 * log10(max(E, 1e-9))
 *   height = clamp((dB + tilt * octaves[bucket] - floor_db) / range_db, 0, 1)
 *
 * with the defaults below: a 36 dB window whose ceiling is 0 dB (E = 1.0 —
 * the level at which the device's automatic gain is about to turn the
 * microphone down, so "full bar" keeps its VU meaning), and a 3 dB/octave
 * treble lift so the mid and high buckets get usable height.
 *
 * Usage (C or C++):
 *
 *     #include <rgbx/rgbx_audio_bars.h>
 *
 *     for (size_t b = 0; b < RGBX_AUDIO_NUM_DISPLAY_BUCKETS; b++) {
 *         float h = rgbx_audio_bar_height(rgbx_inputs.audio_display_bucket[b], b,
 *                                         RGBX_AUDIO_BAR_FLOOR_DB,
 *                                         RGBX_AUDIO_BAR_RANGE_DB,
 *                                         RGBX_AUDIO_BAR_TILT_DB_PER_OCTAVE);
 *         // h is 0..1: draw h * rows of the bar
 *     }
 *
 * Smooth `h` yourself if you want the built-in look (it runs an exponential
 * moving average on the HEIGHT, weight 0.3, three steps per analysis frame).
 */
#ifndef RGBX_AUDIO_BARS_H
#define RGBX_AUDIO_BARS_H

#include <math.h>
#include <rgbx/rgbx_api.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Default floor: bucket power (dB) at which a bar starts to light. */
#define RGBX_AUDIO_BAR_FLOOR_DB (-36.0f)

/** @brief Default range: dB from an empty bar to a full one (3 dB per row on
 *  the 12-row proto0 panel). floor + range = 0 dB = E 1.0. */
#define RGBX_AUDIO_BAR_RANGE_DB (36.0f)

/** @brief Default treble lift: dB added per octave above bucket 0 (pink-noise
 *  slope). Bucket 19 (2.9 kHz, 4.72 octaves up) gets +14 dB. 0 disables it. */
#define RGBX_AUDIO_BAR_TILT_DB_PER_OCTAVE (3.0f)

/** @brief Power below this reads as -90 dB, so a silent bucket stays finite. */
#define RGBX_AUDIO_BAR_POWER_FLOOR (1e-9f)

/**
 * @brief log2(centre_b / centre_0) for each display bucket.
 *
 * Bucket 0 (bins 2–5, 109 Hz) is the reference. The centre frequencies come
 * from the firmware's bucket bin table (audio_display_bucket_start/end in the
 * DSP); the firmware's own test recomputes this array from that table.
 */
static const float rgbx_audio_bar_octaves[RGBX_AUDIO_NUM_DISPLAY_BUCKETS] = {
    0.0000f, 1.0995f, 1.5850f, 1.8931f, 2.1468f, 2.3626f, 2.6521f, 2.9206f, 3.1234f, 3.3424f,
    3.5677f, 3.7318f, 3.8791f, 4.0000f, 4.0753f, 4.1584f, 4.2907f, 4.4500f, 4.5935f, 4.7240f};

/**
 * @brief Bucket power to dB, floored at RGBX_AUDIO_BAR_POWER_FLOOR.
 * @param energy One `audio_display_bucket[]` value (mean bin power).
 * @return 10*log10(energy), or -90 for zero, negative or NaN input.
 */
static inline float rgbx_audio_bar_power_db(float energy) {
    /* `!(x > floor)` rather than `x <= floor`: NaN fails every comparison, so
     * this one form catches zero, negative AND non-finite input. */
    if (!(energy > RGBX_AUDIO_BAR_POWER_FLOOR)) {
        energy = RGBX_AUDIO_BAR_POWER_FLOOR;
    }
    /* 10*log10(x) = (10 / ln 10) * ln(x); logf is the allowed import. */
    return 4.3429448f * logf(energy);
}

/**
 * @brief Bar-height fraction for one bucket under a dB window.
 * @param energy One `audio_display_bucket[]` value (mean bin power).
 * @param bucket Its index, 0 .. RGBX_AUDIO_NUM_DISPLAY_BUCKETS-1 (an index past
 *               the table gets no tilt rather than reading off its end).
 * @param floor_db Power (dB) at which the bar starts to light.
 * @param range_db dB from empty to full; values <= 0 are treated as 1.
 * @param tilt_db_per_octave Treble lift per octave above bucket 0.
 * @return Height in [0, 1]; exactly 0 for zero, negative or NaN energy.
 */
static inline float rgbx_audio_bar_height(float energy, size_t bucket, float floor_db,
                                          float range_db, float tilt_db_per_octave) {
    float tilt, range, h;
    /* No signal, no bar — whatever the window. Without this the -90 dB log
     * floor plus a large tilt and a low floor would draw a bar out of nothing. */
    if (!(energy > 0.0f)) {
        return 0.0f;
    }
    tilt = (bucket < RGBX_AUDIO_NUM_DISPLAY_BUCKETS)
               ? tilt_db_per_octave * rgbx_audio_bar_octaves[bucket]
               : 0.0f;
    range = (range_db > 0.0f) ? range_db : 1.0f;
    h = (rgbx_audio_bar_power_db(energy) + tilt - floor_db) / range;
    if (!(h > 0.0f)) { /* also NaN, should any window value be non-finite */
        return 0.0f;
    }
    if (h > 1.0f) {
        return 1.0f;
    }
    return h;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RGBX_AUDIO_BARS_H */
