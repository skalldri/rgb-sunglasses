#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AUDIO_NUM_BANDS 4
#define AUDIO_FFT_SIZE 512

/* 20 VU-meter-inspired buckets covering bins 2–96 (~62 Hz–3 kHz).
 * More granular at lower frequencies, coarser above 1.5 kHz.
 * Used for display visualisation; beat detection uses AUDIO_NUM_BANDS. */
#define AUDIO_NUM_DISPLAY_BUCKETS 20

/**
 * @brief Runtime-tunable beat-detection parameters (Level 3 spectral flux ODF).
 *
 * Decouples audio_dsp.cpp from any concrete BT/Settings-backed implementation so the
 * existing native_sim ztest suite (fw/tests/sound/audio_dsp/) keeps working with zero
 * changes: it never calls audio_dsp_set_config_provider(), so it always sees the
 * built-in DefaultAudioDspConfigProvider's historical constant values. The real
 * firmware path injects a BT-backed provider (see AudioConfig in audio_config.h) via
 * audio_dsp_bind_default_bt_dependencies().
 */
class AudioDspConfigProvider {
   public:
    virtual ~AudioDspConfigProvider() = default;

    /** Log-compression factor: log1p(GAMMA * energy). */
    virtual float getFluxGamma() = 0;
    /** Set (clamped by the implementation) — used by the "sound dsp set" shell path. */
    virtual void setFluxGamma(float value) = 0;

    /** Minimum flux to prevent false positives on silence. */
    virtual float getBeatFluxFloor() = 0;
    /** Set (clamped by the implementation). */
    virtual void setBeatFluxFloor(float value) = 0;

    /** Adaptive threshold multiplier: mean + alpha * sigma (threshold mode 0 only). */
    virtual float getBeatAlpha() = 0;
    /** Set (clamped by the implementation). */
    virtual void setBeatAlpha(float value) = 0;

    /** Minimum frames between beats per band. */
    virtual uint32_t getBeatRefractoryFrames() = 0;
    /** Set (clamped to [0, 255] to fit the uint8_t per-band counter). */
    virtual void setBeatRefractoryFrames(uint32_t value) = 0;

    /** Additive offset above the running median (threshold mode 1 only).
     *
     * NOTE this is an ABSOLUTE flux offset applied identically to every band,
     * while the bands' flux scales differ by more than an order of magnitude
     * (hardware-measured: band 0 peaks near 3.5, band 3 near 0.2). One delta
     * therefore cannot suit all four — at 0.10, band 3 fired once in 300
     * frames where mode 0 fired 37 times. Mode 0's alpha does not have this
     * problem because sigma scales with each band. Any future work that makes
     * mode 1 the default needs a per-band delta (or a normalized flux). */
    virtual float getSfDelta() = 0;
    /** Set (clamped by the implementation). */
    virtual void setSfDelta(float value) = 0;

    /** Adaptive-threshold shape: 0 = mean + alpha*sigma, 1 = median + sfDelta. */
    virtual uint32_t getThresholdMode() = 0;
    /** Set (clamped to [0, 1]). */
    virtual void setThresholdMode(uint32_t value) = 0;
};

/** Threshold shape selector values for get/setThresholdMode(). */
#define AUDIO_THRESHOLD_MODE_MEAN_SIGMA 0u
#define AUDIO_THRESHOLD_MODE_MEDIAN_DELTA 1u

/**
 * @brief Sets the provider audio_dsp_process() reads beat-detection parameters from.
 *
 * Pass nullptr to revert to the built-in default (historical #define values).
 */
void audio_dsp_set_config_provider(AudioDspConfigProvider *provider);

/**
 * @brief Returns the provider currently in effect (never nullptr - falls back to the
 * built-in default). Lets the "sound dsp" shell commands read/write the same values
 * audio_dsp_process() uses without knowing which concrete provider is installed.
 */
AudioDspConfigProvider *audio_dsp_get_config_provider(void);

/**
 * @brief Injects the real BT-backed config provider. Implemented in audio_config.cpp
 * (declared here, BT-framework-free, so callers like sound.cpp don't need to include
 * any BT headers - mirrors the declare-in-plain-header/implement-in-BT-file split used
 * by e.g. beat_animation_bind_default_bt_dependencies()).
 */
void audio_dsp_bind_default_bt_dependencies();

struct audio_analysis_result {
    float band_energy[AUDIO_NUM_BANDS];
    float band_flux[AUDIO_NUM_BANDS]; /* half-wave-rectified log spectral flux (the ODF) */
    /* The two threshold-statistic slots are MODE-DEPENDENT (struct layout is
     * deliberately unchanged so the msgq, tap format, extension ABI and every
     * consumer stay binary-compatible across the mode switch):
     *   mode 0 (mean+alpha*sigma): band_mean = history mean, band_sigma = history std-dev
     *   mode 1 (median+sfDelta):   band_mean = history MEDIAN, band_sigma = the
     *                              resulting THRESHOLD (median + sfDelta)
     * In both modes the fire test is `flux > band_sigma-as-threshold` in mode 1
     * and `flux > band_mean + alpha*band_sigma` in mode 0, so a consumer that
     * wants to plot the threshold must know the mode (fw/tools/beat_lab/report.py
     * reads threshold_mode from the #PARAMS line for exactly this reason). */
    float band_mean[AUDIO_NUM_BANDS];
    float band_sigma[AUDIO_NUM_BANDS];
    bool beat[AUDIO_NUM_BANDS];

    /* Mean power per display bucket, filled after beat detection. */
    float display_bucket_energy[AUDIO_NUM_DISPLAY_BUCKETS];

    uint32_t seq;
};

/* Call once before the first audio_dsp_process() call. */
void audio_dsp_init(void);

/* Process one 512-sample int16 PCM block. Result written to *out. */
void audio_dsp_process(const int16_t *pcm, uint32_t seq, struct audio_analysis_result *out);

/* Reset all internal beat-detection history (flux buffers, refractory counters,
 * previous-frame state). For AGC gain changes prefer
 * audio_dsp_compensate_gain_change() below — a full reset blinds the adaptive
 * threshold for the next HISTORY_LEN frames (~1 s), which was a root cause of
 * issue #264's poor beat quality (the AGC stepped every few hundred ms during
 * music, so the detector ran chronically history-less). */
void audio_dsp_reset_history(void);

/**
 * @brief Amplitude (RMS-domain) scale factor for a PDM gain change of `steps`
 * register steps: 10^(0.025·steps). One register step = 0.5 dB of amplitude —
 * this pair of helpers is the single authoritative encoding of that fact;
 * every consumer (detector compensation, AGC RMS-window rescale, the replay
 * simulator) must use them instead of re-deriving constants. Header-inline so
 * Zephyr-free consumers (AgcController and its standalone test suite) get them
 * without linking audio_dsp.cpp/CMSIS-DSP. Loop-multiplied per-step constants,
 * no powf (float pow/printf support is compiled out firmware-wide).
 */
static inline float audio_dsp_gain_amplitude_ratio(int steps) {
    const float kStepUp = 1.0592537f;   /* 10^0.025  */
    const float kStepDown = 0.9440609f; /* 10^-0.025 */
    float ratio = 1.0f;
    for (int i = 0; i < (steps > 0 ? steps : -steps); i++) {
        ratio *= (steps > 0) ? kStepUp : kStepDown;
    }
    return ratio;
}

/** @brief Power (energy/magnitude²-domain) scale factor: 10^(0.05·steps) —
 * band energy is power (magnitude²), so one 0.5 dB amplitude step scales it by
 * 10^(2·0.5/20). */
static inline float audio_dsp_gain_power_ratio(int steps) {
    const float kStepUp = 1.1220185f;   /* 10^0.05  */
    const float kStepDown = 0.8912509f; /* 10^-0.05 */
    float ratio = 1.0f;
    for (int i = 0; i < (steps > 0 ? steps : -steps); i++) {
        ratio *= (steps > 0) ? kStepUp : kStepDown;
    }
    return ratio;
}

/**
 * @brief Make flux continuous across a PDM gain change of `steps` register
 * steps (signed; 1 step = 0.5 dB amplitude), instead of resetting history.
 *
 * Scales only the previous-frame energy into the new gain domain (exact at all
 * signal levels — the state is linear power); flux history, threshold
 * statistics, and refractory counters survive. |steps| > 4 (a > 2 dB jump, e.g.
 * a manual "sound agc gain" change) falls back to audio_dsp_reset_history().
 *
 * ORDERING CONTRACT: call AFTER processing the last block captured at the OLD
 * gain and after writing the new gain to the hardware — i.e. between
 * audio_dsp_process() of the pre-step block and of the first post-step block.
 * Calling it before processing an old-gain block injects a false flux of
 * ln(10^(0.05·|steps|)) ≈ 0.115/step on that frame (a spurious beat per gain
 * step — the exact failure this API exists to remove).
 */
void audio_dsp_compensate_gain_change(int steps);
