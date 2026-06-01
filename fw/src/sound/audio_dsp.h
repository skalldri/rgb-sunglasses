#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AUDIO_NUM_BANDS 4
#define AUDIO_FFT_SIZE 512

/* 20 VU-meter-inspired buckets covering bins 2–96 (~62 Hz–3 kHz).
 * More granular at lower frequencies, coarser above 1.5 kHz.
 * Used for display visualisation; beat detection uses AUDIO_NUM_BANDS. */
#define AUDIO_NUM_DISPLAY_BUCKETS 20

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
