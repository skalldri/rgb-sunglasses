#pragma once

#include <rgbx/rgbx_audio_bars.h>
#include <sound/audio_dsp.h>
#include <sound/audio_param_table.h>

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
 * The formula and the octave table are the SDK's rgbx/rgbx_audio_bars.h, so extensions
 * draw the same bars the firmware does; this header adds only what is firmware-specific:
 *
 * - the window comes from the tunables in audio_param_table.h (floor / range / tilt);
 * - the legacy `audio/fft_energy_scale` parameter is reinterpreted as a relative gain,
 *   0 dB at its own default (kFftBarEnergyScaleUnity — derived from the table, not
 *   typed). A persisted value, or an older app's "Bar height" slider, keeps doing
 *   something sensible: larger is taller. The gain is folded into the floor
 *   (fft_bar_effective_floor_db) so the per-bucket work is one SDK call.
 *
 * The animation smooths the resulting HEIGHT (not the linear power) with its per-frame
 * EMA, so attack and release are symmetric in dB and an over-ceiling spike contributes
 * exactly 1.0 — no overshoot memory holding the bar pinned after the spike is gone.
 *
 * Header-only and Zephyr/BT-free (same seam idiom as audio_frame_fold.h): the animation,
 * its native_sim DI suite, the fw/tests/animations/fft_bar_mapping suite and the host
 * vector generator (fw/tools/gen_fft_bar_vectors.cpp, which pins the app's TypeScript
 * mirror) all compile exactly this.
 */

struct FftBarWindow {
    float floorDb;         /* dB of bucket power at which a bar starts to light */
    float rangeDb;         /* dB from empty to full */
    float tiltDbPerOctave; /* pink compensation, added per octave above bucket 0 */
    float energyScale;     /* legacy `audio/fft_energy_scale`; unity == 0 dB gain */
};

/* The `audio/fft_energy_scale` value that maps to 0 dB of gain: its own table default, so
 * a virgin board and a board whose persisted value was never touched both get exactly the
 * documented window — true by construction, not by assertion. */
inline constexpr float kFftBarEnergyScaleUnity = audioParamDefaultF<kAudioParamFftEnergyScale>();

/* The SDK header carries the defaults as literals because it cannot see the table; pin
 * them here so the two never drift (this is the one place the SDK and the table meet). */
static_assert(RGBX_AUDIO_BAR_FLOOR_DB == audioParamDefaultF<kAudioParamFftFloorDb>(),
              "rgbx_audio_bars.h floor default has drifted from audio_param_table.h");
static_assert(RGBX_AUDIO_BAR_RANGE_DB == audioParamDefaultF<kAudioParamFftRangeDb>(),
              "rgbx_audio_bars.h range default has drifted from audio_param_table.h");
static_assert(RGBX_AUDIO_BAR_TILT_DB_PER_OCTAVE == audioParamDefaultF<kAudioParamFftTiltDbOct>(),
              "rgbx_audio_bars.h tilt default has drifted from audio_param_table.h");

/* Same floor the SDK uses: zero, negative and NaN power all read as −90 dB. */
inline constexpr float kFftBarPowerFloor = RGBX_AUDIO_BAR_POWER_FLOOR;

/** Bucket power → dB, floored at kFftBarPowerFloor (so 0, negative and NaN read as −90). */
inline float fft_bar_power_db(float energy) { return rgbx_audio_bar_power_db(energy); }

/** Legacy energy-scale parameter → relative gain in dB (0 at kFftBarEnergyScaleUnity). */
inline float fft_bar_gain_db(float energyScale) {
    /* The table clamp (0.1..1000) already excludes these; the guard is for a caller that
     * bypassed it, e.g. a test or a default-constructed window. */
    if (!(energyScale > 0.0f)) {
        return 0.0f;
    }
    return 10.0f * log10f(energyScale / kFftBarEnergyScaleUnity);
}

/** The window's floor with the legacy gain folded in: (dB + tilt + gain − floor) / range
 *  is (dB + tilt − (floor − gain)) / range. Compute once per frame, not per bucket. */
inline float fft_bar_effective_floor_db(const FftBarWindow &w) {
    return w.floorDb - fft_bar_gain_db(w.energyScale);
}

/** Bar-height fraction in [0, 1] for one bucket's power under `w`. */
inline float fft_bar_height(float energy, size_t bucket, const FftBarWindow &w) {
    return rgbx_audio_bar_height(energy, bucket, fft_bar_effective_floor_db(w), w.rangeDb,
                                 w.tiltDbPerOctave);
}
