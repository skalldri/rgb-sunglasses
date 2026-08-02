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

    /** Adaptive threshold multiplier: mean + alpha * sigma. */
    virtual float getBeatAlpha() = 0;
    /** Set (clamped by the implementation). */
    virtual void setBeatAlpha(float value) = 0;

    /** Minimum frames between beats per band. */
    virtual uint32_t getBeatRefractoryFrames() = 0;
    /** Set (clamped to [0, 255] to fit the uint8_t per-band counter). */
    virtual void setBeatRefractoryFrames(uint32_t value) = 0;
};

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
    float band_flux[AUDIO_NUM_BANDS];  /* half-wave-rectified log spectral flux (the ODF) */
    float band_mean[AUDIO_NUM_BANDS];  /* history mean, for noise-floor tuning */
    float band_sigma[AUDIO_NUM_BANDS]; /* history std-dev, for noise-floor tuning */
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
 * simulator) must use them instead of re-deriving constants.
 */
float audio_dsp_gain_amplitude_ratio(int steps);

/** @brief Power (energy/magnitude²-domain) scale factor: 10^(0.05·steps). */
float audio_dsp_gain_power_ratio(int steps);

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
