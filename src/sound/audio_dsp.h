#pragma once
#include <stdint.h>
#include <stdbool.h>

#define AUDIO_NUM_BANDS 4
#define AUDIO_FFT_SIZE  512

struct audio_analysis_result {
	float band_energy[AUDIO_NUM_BANDS];
	bool  beat[AUDIO_NUM_BANDS];
	uint32_t seq;
};

/* Call once before the first audio_dsp_process() call. */
void audio_dsp_init(void);

/* Process one 512-sample int16 PCM block. Result written to *out. */
void audio_dsp_process(const int16_t *pcm, uint32_t seq,
		       struct audio_analysis_result *out);
