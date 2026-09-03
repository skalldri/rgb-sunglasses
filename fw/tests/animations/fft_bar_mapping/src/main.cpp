/* Unit tests for the FFT Bars display mapping (fw/src/animations/fft_bar_mapping.h).
 *
 * The header is header-only and Zephyr-free, so this suite links nothing. The same
 * numbers are emitted by fw/tools/gen_fft_bar_vectors.cpp into a JSON fixture that pins
 * the companion app's TypeScript mirror (app/services/fft-bar-mapping.ts) — so a change
 * here that moves a spot value must regenerate that fixture too. */
#include <math.h>
#include <zephyr/ztest.h>

#include "animations/fft_bar_mapping.h"
#include "sound/audio_param_table.h"

namespace {

/* The table defaults, spelled out so a retune is a deliberate two-place edit. */
constexpr FftBarWindow kDefaults = {-36.0f, 36.0f, 3.0f, 20.0f};

constexpr float kTol = 1e-4f;

}  // namespace

ZTEST_SUITE(fft_bar_mapping, NULL, NULL, NULL, NULL, NULL);

/* ── Defaults are the table's ───────────────────────────────────────────── */

ZTEST(fft_bar_mapping, test_defaults_match_param_table) {
    zassert_equal(kDefaults.floorDb, audioParamDefaultF<kAudioParamFftFloorDb>());
    zassert_equal(kDefaults.rangeDb, audioParamDefaultF<kAudioParamFftRangeDb>());
    zassert_equal(kDefaults.tiltDbPerOctave, audioParamDefaultF<kAudioParamFftTiltDbOct>());
    zassert_equal(kDefaults.energyScale, audioParamDefaultF<kAudioParamFftEnergyScale>());
    /* The SDK header's literal defaults (it cannot see the table) — static_asserted in
     * fft_bar_mapping.h, pinned here too so the record survives a refactor. */
    zassert_equal(RGBX_AUDIO_BAR_FLOOR_DB, kDefaults.floorDb);
    zassert_equal(RGBX_AUDIO_BAR_RANGE_DB, kDefaults.rangeDb);
    zassert_equal(RGBX_AUDIO_BAR_TILT_DB_PER_OCTAVE, kDefaults.tiltDbPerOctave);
}

/* The firmware mapping is the SDK mapping plus the legacy gain: with the gain at unity
 * the two must agree exactly, and a gain must equal a floor shift. */
ZTEST(fft_bar_mapping, test_matches_sdk_helper) {
    const float probes[] = {1e-6f, 3e-5f, 1e-3f, 0.039f, 0.3f, 1.0f, 5.0f};
    for (float e : probes) {
        for (size_t b = 0; b < AUDIO_NUM_DISPLAY_BUCKETS; b += 5) {
            zassert_equal(fft_bar_height(e, b, kDefaults),
                          rgbx_audio_bar_height(e, b, RGBX_AUDIO_BAR_FLOOR_DB,
                                                RGBX_AUDIO_BAR_RANGE_DB,
                                                RGBX_AUDIO_BAR_TILT_DB_PER_OCTAVE),
                          "E = %g bucket %zu: firmware and SDK mapping differ", (double)e, b);
        }
    }
    FftBarWindow louder = kDefaults;
    louder.energyScale = 200.0f; /* +10 dB */
    zassert_within(fft_bar_effective_floor_db(louder), kDefaults.floorDb - 10.0f, kTol);
    zassert_within(fft_bar_height(0.01f, 0, louder),
                   rgbx_audio_bar_height(0.01f, 0, kDefaults.floorDb - 10.0f, kDefaults.rangeDb,
                                         kDefaults.tiltDbPerOctave),
                   kTol);
}

/* ── Power → dB ──────────────────────────────────────────────────────────── */

ZTEST(fft_bar_mapping, test_power_db_is_finite_for_degenerate_input) {
    const float probes[] = {0.0f, -1.0f, -1e30f, NAN, -NAN, 1e-30f};
    for (float p : probes) {
        const float db = fft_bar_power_db(p);
        zassert_true(isfinite(db), "power %g gave non-finite dB", (double)p);
        zassert_within(db, 10.0f * log10f(kFftBarPowerFloor), kTol,
                       "degenerate power %g must read as the floor", (double)p);
    }
    zassert_within(fft_bar_power_db(1.0f), 0.0f, kTol, "E = 1.0 is 0 dB");
    zassert_within(fft_bar_power_db(0.1f), -10.0f, kTol);
    zassert_within(fft_bar_power_db(100.0f), 20.0f, kTol);
}

/* ── Legacy energy scale → relative gain ─────────────────────────────────── */

ZTEST(fft_bar_mapping, test_energy_scale_is_relative_gain) {
    zassert_within(fft_bar_gain_db(20.0f), 0.0f, kTol, "unity at the table default");
    zassert_within(fft_bar_gain_db(200.0f), 10.0f, kTol);
    zassert_within(fft_bar_gain_db(2.0f), -10.0f, kTol);
    /* Outside the clamp range, but must not produce NaN or -inf. */
    zassert_equal(fft_bar_gain_db(0.0f), 0.0f);
    zassert_equal(fft_bar_gain_db(-5.0f), 0.0f);
    zassert_equal(fft_bar_gain_db(NAN), 0.0f);
}

/* ── The window ──────────────────────────────────────────────────────────── */

ZTEST(fft_bar_mapping, test_window_endpoints) {
    /* Bucket 0 has no tilt, so the window is exactly floor..floor+range in power dB. */
    const float floorE = powf(10.0f, kDefaults.floorDb / 10.0f);     /* −36 dB */
    const float ceilE = powf(10.0f, (kDefaults.floorDb + kDefaults.rangeDb) / 10.0f); /* 0 dB */
    const float midE = powf(10.0f, (kDefaults.floorDb + kDefaults.rangeDb / 2.0f) / 10.0f);

    zassert_within(fft_bar_height(floorE, 0, kDefaults), 0.0f, kTol, "floor → empty");
    zassert_within(fft_bar_height(ceilE, 0, kDefaults), 1.0f, kTol, "ceiling → full");
    zassert_within(fft_bar_height(midE, 0, kDefaults), 0.5f, kTol, "midpoint → half");
    zassert_equal(fft_bar_height(ceilE * 1000.0f, 0, kDefaults), 1.0f, "far above clamps to 1");
    zassert_equal(fft_bar_height(floorE / 1000.0f, 0, kDefaults), 0.0f, "far below clamps to 0");
}

ZTEST(fft_bar_mapping, test_degenerate_power_is_dark_and_finite) {
    zassert_equal(fft_bar_height(0.0f, 0, kDefaults), 0.0f);
    zassert_equal(fft_bar_height(-1.0f, 0, kDefaults), 0.0f);
    zassert_equal(fft_bar_height(NAN, 0, kDefaults), 0.0f);
    /* No signal draws no bar even under the most permissive window the table allows: the
     * lowest floor, the maximum tilt on the top bucket (+57 dB) and the maximum legacy gain
     * (+17 dB) would otherwise lift the −90 dB log floor above −80 dB. */
    const FftBarWindow permissive = {-80.0f, 6.0f, 12.0f, 1000.0f};
    zassert_equal(fft_bar_height(0.0f, AUDIO_NUM_DISPLAY_BUCKETS - 1, permissive), 0.0f);
    zassert_equal(fft_bar_height(-1.0f, AUDIO_NUM_DISPLAY_BUCKETS - 1, permissive), 0.0f);
    zassert_equal(fft_bar_height(NAN, AUDIO_NUM_DISPLAY_BUCKETS - 1, permissive), 0.0f);
    /* ...while a tiny but real signal is mapped honestly under that same window. */
    zassert_true(fft_bar_height(1e-9f, AUDIO_NUM_DISPLAY_BUCKETS - 1, permissive) > 0.0f);
}

ZTEST(fft_bar_mapping, test_height_is_monotonic_in_power) {
    float prev = -1.0f;
    for (float e = 1e-9f; e <= 1e4f; e *= 1.5f) {
        const float h = fft_bar_height(e, 0, kDefaults);
        zassert_true(h >= prev, "height decreased between %g and the previous probe", (double)e);
        zassert_true(h >= 0.0f && h <= 1.0f, "height out of [0,1] at %g", (double)e);
        prev = h;
    }
}

ZTEST(fft_bar_mapping, test_out_of_range_bucket_gets_no_tilt) {
    /* The animation's kMaxDisplayBuckets (24) exceeds AUDIO_NUM_DISPLAY_BUCKETS (20); a
     * bucket index past the table must fall back to zero tilt, not read off its end. */
    zassert_within(fft_bar_height(0.1f, AUDIO_NUM_DISPLAY_BUCKETS, kDefaults),
                   fft_bar_height(0.1f, 0, kDefaults), kTol);
    zassert_within(fft_bar_height(0.1f, 1000, kDefaults), fft_bar_height(0.1f, 0, kDefaults),
                   kTol);
}

/* ── Tilt ────────────────────────────────────────────────────────────────── */

ZTEST(fft_bar_mapping, test_tilt_offsets) {
    zassert_equal(rgbx_audio_bar_octaves[0], 0.0f, "bucket 0 is the tilt reference");
    for (size_t b = 1; b < AUDIO_NUM_DISPLAY_BUCKETS; b++) {
        zassert_true(rgbx_audio_bar_octaves[b] > rgbx_audio_bar_octaves[b - 1],
                     "octave table must be strictly increasing (bucket %zu)", b);
    }
    /* Bucket 19 (2.9 kHz) is 4.72 octaves above bucket 0 (109 Hz). */
    zassert_within(rgbx_audio_bar_octaves[AUDIO_NUM_DISPLAY_BUCKETS - 1], 4.724f, 0.01f);

    /* Same power, higher bucket → taller bar, by tilt × octaves over the range. */
    const float e = 0.01f; /* −20 dB */
    const float h0 = fft_bar_height(e, 0, kDefaults);
    const float h19 = fft_bar_height(e, 19, kDefaults);
    zassert_within(h19 - h0, kDefaults.tiltDbPerOctave * rgbx_audio_bar_octaves[19] / kDefaults.rangeDb,
                   kTol);

    /* tilt = 0 makes every bucket render identically. */
    FftBarWindow flat = kDefaults;
    flat.tiltDbPerOctave = 0.0f;
    for (size_t b = 0; b < AUDIO_NUM_DISPLAY_BUCKETS; b++) {
        zassert_within(fft_bar_height(e, b, flat), fft_bar_height(e, 0, flat), kTol,
                       "bucket %zu differs with tilt 0", b);
    }
}

/* The octave table is hand-transcribed (log2 is not constexpr); recompute it from the
 * bucket bin layout it claims to describe. */
ZTEST(fft_bar_mapping, test_octave_table_matches_bucket_bins) {
    const float centre0 =
        (float)(audio_display_bucket_start[0] + audio_display_bucket_end[0]) / 2.0f;
    for (size_t b = 0; b < AUDIO_NUM_DISPLAY_BUCKETS; b++) {
        const float centre =
            (float)(audio_display_bucket_start[b] + audio_display_bucket_end[b]) / 2.0f;
        const float octaves = log2f(centre / centre0);
        zassert_within(rgbx_audio_bar_octaves[b], octaves, 0.01f,
                       "bucket %zu: table says %g octaves, bins say %g", b,
                       (double)rgbx_audio_bar_octaves[b], (double)octaves);
    }
}

/* ── Spot values shared with the app fixture ─────────────────────────────── */

ZTEST(fft_bar_mapping, test_measured_spot_values) {
    /* Bucket 0's median on the 2026-09-03 TV capture: 0.039 → −14.1 dB → 61 % of the bar. */
    zassert_within(fft_bar_height(0.039f, 0, kDefaults), 0.6086f, 1e-3f);
    /* Its maximum, 1.87 (+2.7 dB): above the ceiling, clamps. */
    zassert_equal(fft_bar_height(1.87f, 0, kDefaults), 1.0f);
    /* Bucket 19's median, 3e-5 (−45.2 dB) + 14.2 dB tilt → 0.137 (1–2 rows of 12). */
    zassert_within(fft_bar_height(3e-5f, 19, kDefaults), 0.1373f, 2e-3f);
    /* The old linear mapping's saturation point (E = 0.05) is now only 62 % on bucket 0. */
    zassert_within(fft_bar_height(0.05f, 0, kDefaults), 0.6386f, 1e-3f);
}

ZTEST(fft_bar_mapping, test_energy_scale_shifts_the_window) {
    const float e = 0.01f;
    FftBarWindow louder = kDefaults;
    louder.energyScale = 200.0f; /* +10 dB */
    zassert_within(fft_bar_height(e, 0, louder) - fft_bar_height(e, 0, kDefaults),
                   10.0f / kDefaults.rangeDb, kTol);
    FftBarWindow quieter = kDefaults;
    quieter.energyScale = 2.0f; /* −10 dB */
    zassert_within(fft_bar_height(e, 0, kDefaults) - fft_bar_height(e, 0, quieter),
                   10.0f / kDefaults.rangeDb, kTol);
}
