/* Unit tests for the audio parameter table (fw/src/sound/audio_param_table.h).
 *
 * That header is the single source of truth for the default value, the clamp
 * range and the GATT ordering of all 14 tunable audio parameters. Before it
 * existed, each of those facts was written out FOUR times — in
 * DefaultAudioDspConfigProvider (audio_dsp.cpp), DefaultAgcConfigProvider
 * (sound.cpp), AudioConfig (audio_config.cpp) and the native_sim replay app's
 * Env*Provider pair — with nothing checking that the copies agreed.
 *
 * The most important thing in this file is kHistoricalParams below: a
 * transcription of the literals as they stood in those four copies immediately
 * before the table replaced them. It is what proves the consolidation was
 * behaviour-preserving, and it is the reason a future retune has to be a
 * deliberate two-place edit (table + this test) rather than an accident.
 *
 * The header is header-only and Zephyr-free, so this suite needs no fixture and
 * links nothing. */
#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>

#include "audio_param_table.h"

namespace {

/* ---------------------------------------------------------------------------
 * The historical record.
 *
 * Transcribed from the pre-consolidation sources. ORDER is the GATT declaration
 * order in audio_config.cpp's BtGattServer argument list, which is also the
 * positional characteristic index BtGattServer bakes into each UUID
 * (composeAutoCharacteristicUuid) — so index N here is the characteristic whose
 * UUID ends ...000N, and reordering this array would be reordering the BLE
 * wire contract.
 * ------------------------------------------------------------------------- */
struct HistoricalParam {
    const char *key;
    const char *label;
    bool isFloat;
    float def;
    float min;
    float max;
};

constexpr HistoricalParam kHistoricalParams[] = {
    /*  0 */ {"audio/flux_gamma", "Flux Gamma", true, 1000.0f, 1.0f, 100000.0f},
    /*  1 */ {"audio/beat_flux_floor", "Beat Flux Floor", true, 0.08f, 0.0f, 1.0f},
    /*  2 */ {"audio/beat_alpha", "Beat Alpha", true, 0.3f, 0.1f, 20.0f},
    /*  3 */ {"audio/beat_refractory_frames", "Beat Refractory Frames", false, 5.0f, 0.0f, 255.0f},
    /*  4 */ {"audio/agc_target_low", "AGC Target Low", true, 0.002f, 0.001f, 0.1f},
    /*  5 */ {"audio/agc_target_high", "AGC Target High", true, 0.05f, 0.02f, 0.5f},
    /*  6 */ {"audio/agc_rate_limit_frames", "AGC Rate Limit Frames", false, 10.0f, 1.0f, 100.0f},
    /*  7 */ {"audio/fft_smoothing_coeff", "FFT Smoothing Coeff", true, 0.3f, 0.0f, 1.0f},
    /*  8 */ {"audio/fft_energy_scale", "FFT Energy Scale", true, 20.0f, 0.1f, 1000.0f},
    /*  9 */ {"audio/agc_attack_frames", "AGC Attack Frames", false, 3.0f, 1.0f, 20.0f},
    /* 10 */ {"audio/agc_release_frames", "AGC Release Frames", false, 15.0f, 1.0f, 100.0f},
    /* 11 */ {"audio/noise_gate_rms", "AGC Noise Gate RMS", true, 0.0006f, 0.0f, 0.02f},
    /* 12 */ {"audio/sf_delta", "Beat SF Delta", true, 0.10f, 0.0f, 2.0f},
    /* 13 */ {"audio/threshold_mode", "Beat Threshold Mode", false, 0.0f, 0.0f, 1.0f},
};

constexpr size_t kHistoricalCount = sizeof(kHistoricalParams) / sizeof(kHistoricalParams[0]);

}  // namespace

ZTEST_SUITE(audio_param_table, NULL, NULL, NULL, NULL, NULL);

/* ── The regression net ──────────────────────────────────────────────────── */

ZTEST(audio_param_table, test_count_matches_history) {
    zassert_equal(kAudioParamCount, kHistoricalCount,
                  "parameter count changed: %zu vs %zu historical. Adding a parameter is fine, "
                  "but it must be APPENDED (positional GATT UUIDs) and recorded here.",
                  (size_t)kAudioParamCount, kHistoricalCount);
}

ZTEST(audio_param_table, test_ranges_match_history_exactly) {
    for (size_t i = 0; i < kAudioParamCount; i++) {
        const AudioParamSpec &p = kAudioParams[i];
        const HistoricalParam &h = kHistoricalParams[i];

        zassert_true(strcmp(p.key, h.key) == 0, "index %zu key: got '%s', expected '%s'", i, p.key,
                     h.key);
        zassert_true(strcmp(p.label, h.label) == 0, "index %zu label: got '%s', expected '%s'", i,
                     p.label, h.label);

        /* Exact float equality is intended: these are the literals the firmware
         * used to carry, and "close enough" is not a property worth having here. */
        zassert_equal(p.def, h.def, "index %zu (%s) default drifted", i, h.key);
        zassert_equal(p.min, h.min, "index %zu (%s) min drifted", i, h.key);
        zassert_equal(p.max, h.max, "index %zu (%s) max drifted", i, h.key);
    }
}

ZTEST(audio_param_table, test_types_match_history) {
    for (size_t i = 0; i < kAudioParamCount; i++) {
        const bool tableIsFloat = (kAudioParams[i].type == AudioParamType::F32);
        zassert_equal(tableIsFloat, kHistoricalParams[i].isFloat,
                      "index %zu (%s) float-vs-integer type drifted", i, kHistoricalParams[i].key);
    }
}

/* ── Structural invariants (also static_asserted; pinned here for the record) ─ */

ZTEST(audio_param_table, test_structural_invariants) {
    zassert_true(audioParamTableSelfCheck(), "table self-check failed");

    for (size_t i = 0; i < kAudioParamCount; i++) {
        const AudioParamSpec &p = kAudioParams[i];
        zassert_true(p.min <= p.def && p.def <= p.max, "index %zu (%s) default outside range", i,
                     p.key);
        zassert_true(p.step > 0.0f, "index %zu (%s) step must be positive", i, p.key);
        zassert_not_null(p.unit, "index %zu (%s) unit must be \"\", never null", i, p.key);
        zassert_true((p.type == AudioParamType::ENUM) == (p.enumLabels != nullptr),
                     "index %zu (%s) enum labels present iff type is ENUM", i, p.key);
    }
}

ZTEST(audio_param_table, test_keys_and_labels_are_unique) {
    for (size_t i = 0; i < kAudioParamCount; i++) {
        for (size_t j = i + 1; j < kAudioParamCount; j++) {
            zassert_true(strcmp(kAudioParams[i].key, kAudioParams[j].key) != 0,
                         "duplicate settings key '%s' at %zu and %zu", kAudioParams[i].key, i, j);
            zassert_true(strcmp(kAudioParams[i].label, kAudioParams[j].label) != 0,
                         "duplicate CUD label '%s' at %zu and %zu", kAudioParams[i].label, i, j);
        }
    }
}

/* Settings keys are persisted in NVS and characteristic labels are shown in the
 * companion app; both are effectively wire format. */
ZTEST(audio_param_table, test_keys_are_namespaced) {
    for (size_t i = 0; i < kAudioParamCount; i++) {
        zassert_true(strncmp(kAudioParams[i].key, "audio/", 6) == 0,
                     "index %zu key '%s' is not under the audio/ settings subtree", i,
                     kAudioParams[i].key);
    }
}

/* ── Clamp behaviour ─────────────────────────────────────────────────────── */

ZTEST(audio_param_table, test_clamp_float_saturates_at_bounds) {
    zassert_equal(audioParamClampF<kAudioParamBeatAlpha>(50.0f), 20.0f);
    zassert_equal(audioParamClampF<kAudioParamBeatAlpha>(0.0f), 0.1f);
    zassert_equal(audioParamClampF<kAudioParamBeatAlpha>(1.5f), 1.5f);

    /* Bounds themselves are inclusive. */
    zassert_equal(audioParamClampF<kAudioParamBeatAlpha>(0.1f), 0.1f);
    zassert_equal(audioParamClampF<kAudioParamBeatAlpha>(20.0f), 20.0f);

    zassert_equal(audioParamClampF<kAudioParamNoiseGateRms>(0.0f), 0.0f,
                  "gate must accept exactly 0 — that is how the gate is disabled");
}

ZTEST(audio_param_table, test_clamp_integer_saturates_at_bounds) {
    /* 255 is a hard limit: the per-band refractory counter in audio_dsp.cpp is uint8_t. */
    zassert_equal(audioParamClampU<kAudioParamBeatRefractoryFrames>(999u), 255u);
    zassert_equal(audioParamClampU<kAudioParamBeatRefractoryFrames>(0u), 0u);

    zassert_equal(audioParamClampU<kAudioParamAgcAttackFrames>(0u), 1u);
    zassert_equal(audioParamClampU<kAudioParamAgcAttackFrames>(99u), 20u);
    zassert_equal(audioParamClampU<kAudioParamAgcAttackFrames>(7u), 7u);

    zassert_equal(audioParamClampU<kAudioParamThresholdMode>(2u), 1u);
    zassert_equal(audioParamClampU<kAudioParamThresholdMode>(0xFFFFFFFFu), 1u);
}

/* The deliberate behaviour change this consolidation introduced.
 *
 * The four previous copies disagreed on NaN: std::clamp(NaN, lo, hi) returns NaN
 * (every comparison is false), while the replay harness's fminf(fmaxf(NaN, lo), hi)
 * returned lo. Neither is acceptable — a NaN threshold makes every comparison in
 * the detector false, silently disabling beat output or the AGC, which is exactly
 * why the shell path already rejected it via parse_finite_float(). The BLE write
 * path had no such guard, so a NaN written over GATT was accepted, persisted to
 * NVS, and reloaded on every boot. */
ZTEST(audio_param_table, test_nan_maps_to_default) {
    zassert_equal(audioParamClampF<kAudioParamBeatAlpha>(NAN),
                  audioParamDefaultF<kAudioParamBeatAlpha>());
    zassert_equal(audioParamClampF<kAudioParamNoiseGateRms>(NAN),
                  audioParamDefaultF<kAudioParamNoiseGateRms>());
    zassert_equal(audioParamClampF<kAudioParamFluxGamma>(-NAN),
                  audioParamDefaultF<kAudioParamFluxGamma>());

    /* And the result is finite, which is the property the DSP actually depends on. */
    for (size_t i = 0; i < kAudioParamCount; i++) {
        zassert_true(isfinite(kAudioParams[i].def), "index %zu default is not finite", i);
    }
}

ZTEST(audio_param_table, test_infinities_saturate) {
    zassert_equal(audioParamClampF<kAudioParamBeatAlpha>(INFINITY), 20.0f);
    zassert_equal(audioParamClampF<kAudioParamBeatAlpha>(-INFINITY), 0.1f);
    zassert_equal(audioParamClampF<kAudioParamNoiseGateRms>(INFINITY), 0.02f);
    zassert_equal(audioParamClampF<kAudioParamNoiseGateRms>(-INFINITY), 0.0f);
}

/* Clamping is idempotent: the getters in AudioConfig clamp on read and write the
 * clamped value back only when it differs, so a non-idempotent clamp would make
 * them rewrite (and re-notify) forever. */
ZTEST(audio_param_table, test_clamp_is_idempotent) {
    const float probes[] = {-1.0e9f, -1.0f, 0.0f, 1.0e-9f, 0.5f, 1.0f, 3.0f, 1.0e9f, NAN, INFINITY};

    for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
        const float once = audioParamClampF<kAudioParamBeatAlpha>(probes[p]);
        zassert_equal(audioParamClampF<kAudioParamBeatAlpha>(once), once,
                      "clamp not idempotent for probe %zu", p);

        const float once2 = audioParamClampF<kAudioParamNoiseGateRms>(probes[p]);
        zassert_equal(audioParamClampF<kAudioParamNoiseGateRms>(once2), once2,
                      "clamp not idempotent for probe %zu", p);
    }
}

/* ── Defaults ────────────────────────────────────────────────────────────── */

ZTEST(audio_param_table, test_defaults_match_history) {
    zassert_equal(audioParamDefaultF<kAudioParamFluxGamma>(), 1000.0f);
    zassert_equal(audioParamDefaultF<kAudioParamBeatFluxFloor>(), 0.08f);
    zassert_equal(audioParamDefaultF<kAudioParamBeatAlpha>(), 0.3f);
    zassert_equal(audioParamDefaultU<kAudioParamBeatRefractoryFrames>(), 5u);
    zassert_equal(audioParamDefaultF<kAudioParamAgcTargetLow>(), 0.002f);
    zassert_equal(audioParamDefaultF<kAudioParamAgcTargetHigh>(), 0.05f);
    zassert_equal(audioParamDefaultU<kAudioParamAgcRateLimitFrames>(), 10u);
    zassert_equal(audioParamDefaultF<kAudioParamFftSmoothingCoeff>(), 0.3f);
    zassert_equal(audioParamDefaultF<kAudioParamFftEnergyScale>(), 20.0f);
    zassert_equal(audioParamDefaultU<kAudioParamAgcAttackFrames>(), 3u);
    zassert_equal(audioParamDefaultU<kAudioParamAgcReleaseFrames>(), 15u);
    zassert_equal(audioParamDefaultF<kAudioParamNoiseGateRms>(), 0.0006f);
    zassert_equal(audioParamDefaultF<kAudioParamSfDelta>(), 0.10f);
    zassert_equal(audioParamDefaultU<kAudioParamThresholdMode>(), 0u);
}

/* Every default must survive its own clamp — otherwise a virgin board would boot
 * with a value the getters immediately rewrite. */
ZTEST(audio_param_table, test_defaults_are_in_range) {
    for (size_t i = 0; i < kAudioParamCount; i++) {
        const AudioParamSpec &p = kAudioParams[i];
        zassert_true(p.def >= p.min && p.def <= p.max, "index %zu (%s) default out of range", i,
                     p.key);
    }
}

/* The *Frames parameters are meaningless without the frame period. */
ZTEST(audio_param_table, test_frame_period) {
    zassert_equal(kAudioParamFrameMs, 32u,
                  "frame period changed — every *Frames default and the app's ms conversion "
                  "must be revisited together");
}

ZTEST(audio_param_table, test_str_eq_helper) {
    zassert_true(audioParamStrEq("abc", "abc"));
    zassert_false(audioParamStrEq("abc", "abd"));
    zassert_false(audioParamStrEq("abc", "ab"));
    zassert_false(audioParamStrEq("ab", "abc"));
    zassert_true(audioParamStrEq("", ""));
}
