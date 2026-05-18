#pragma once
#include <stdint.h>
#include <stdbool.h>

#define AUDIO_NUM_BANDS          4
#define AUDIO_FFT_SIZE           512

/* 14 logarithmically-spaced buckets (bins 2–254, ~62 Hz–8 kHz).
 * Used for display visualisation; beat detection uses AUDIO_NUM_BANDS. */
#define AUDIO_NUM_DISPLAY_BUCKETS 14

struct audio_analysis_result {
	float band_energy[AUDIO_NUM_BANDS];
	float band_mean[AUDIO_NUM_BANDS];   /* history mean, for noise-floor tuning */
	float band_sigma[AUDIO_NUM_BANDS];  /* history std-dev, for noise-floor tuning */
	bool  beat[AUDIO_NUM_BANDS];

	/* Mean power per display bucket, filled after beat detection. */
	float display_bucket_energy[AUDIO_NUM_DISPLAY_BUCKETS];

	uint32_t seq;
};

/* Call once before the first audio_dsp_process() call. */
void audio_dsp_init(void);

/* Process one 512-sample int16 PCM block. Result written to *out. */
void audio_dsp_process(const int16_t *pcm, uint32_t seq,
		       struct audio_analysis_result *out);
