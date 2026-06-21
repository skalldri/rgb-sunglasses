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

    /** Minimum flux to prevent false positives on silence. */
    virtual float getBeatFluxFloor() = 0;

    /** Adaptive threshold multiplier: mean + alpha * sigma. */
    virtual float getBeatAlpha() = 0;

    /** Minimum frames between beats per band. */
    virtual uint32_t getBeatRefractoryFrames() = 0;
};

/**
 * @brief Sets the provider audio_dsp_process() reads beat-detection parameters from.
 *
 * Pass nullptr to revert to the built-in default (historical #define values).
 */
void audio_dsp_set_config_provider(AudioDspConfigProvider *provider);

/**
 * @brief Injects the real BT-backed config provider. Implemented in audio_config.cpp
 * (declared here, BT-framework-free, so callers like sound.cpp don't need to include
 * any BT headers - mirrors the declare-in-plain-header/implement-in-BT-file split used
 * by e.g. beat_animation_bind_default_bt_dependencies()).
 */
void audio_dsp_bind_default_bt_dependencies();

struct audio_analysis_result {
    float band_energy[AUDIO_NUM_BANDS];
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
 * previous-frame state).  Must be called after every AGC gain change: the
 * amplitude discontinuity would otherwise look like a beat onset. */
void audio_dsp_reset_history(void);
