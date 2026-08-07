/*
 * WASM wrapper around the REAL firmware audio DSP (fw/src/sound/audio_dsp.cpp
 * + SDK CMSIS-DSP, generic C paths). The simulator feeds 512-sample 16 kHz
 * PCM blocks through this module so the audio features an extension sees
 * come from the genuine FFT/spectral-flux/beat pipeline, not an imitation.
 *
 * Flat exported buffers keep the JS side free of C struct layout knowledge:
 * write sim_pcm, call sim_process(seq), read the sim_* output arrays.
 * (Same idea as the native_sim replay harness in
 * fw/tests/sound/audio_dsp_replay/, which proves this TU compiles and runs
 * correctly off-target.)
 */

#include <audio_dsp.h>
#include <stdint.h>

extern "C" {

int16_t sim_pcm[AUDIO_FFT_SIZE];

float sim_band_energy[AUDIO_NUM_BANDS];
float sim_band_flux[AUDIO_NUM_BANDS];
/* Mode-dependent semantics — see the audio_analysis_result comment in
 * audio_dsp.h (mode 0: mean/sigma; mode 1: median/threshold). */
float sim_band_mean[AUDIO_NUM_BANDS];
float sim_band_sigma[AUDIO_NUM_BANDS];
uint8_t sim_beat[AUDIO_NUM_BANDS];
float sim_display_bucket[AUDIO_NUM_DISPLAY_BUCKETS];

void sim_init(void) {
    audio_dsp_init();
}

void sim_process(uint32_t seq) {
    struct audio_analysis_result result;
    audio_dsp_process(sim_pcm, seq, &result);
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        sim_band_energy[b] = result.band_energy[b];
        sim_band_flux[b] = result.band_flux[b];
        sim_band_mean[b] = result.band_mean[b];
        sim_band_sigma[b] = result.band_sigma[b];
        sim_beat[b] = result.beat[b] ? 1 : 0;
    }
    for (int i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) {
        sim_display_bucket[i] = result.display_bucket_energy[i];
    }
}

void sim_reset_history(void) {
    audio_dsp_reset_history();
}

} /* extern "C" */
