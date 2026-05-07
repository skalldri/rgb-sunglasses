/* Include all Zephyr headers before arm_math.h to avoid ROUND_UP/ROUND_DOWN
 * macro collision (Zephyr issue #64327). */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <arm_math.h>

#include "audio_dsp.h"

LOG_MODULE_REGISTER(audio_dsp);

#define NUM_FFT_SAMPLES AUDIO_FFT_SIZE
#define NUM_BANDS       AUDIO_NUM_BANDS
#define HISTORY_LEN     32    /* ~1 s at 32 ms/frame */
#define BEAT_ALPHA      2.0f  /* threshold = mean + alpha * sigma */
#define BEAT_REFRACTORY 5     /* minimum frames between beats per band */

/* Sub-band bin boundaries (512-pt FFT at 16 kHz, bin width = 31.25 Hz).
 * Band 0 bass:    bins  1– 6  →  31– 200 Hz (kick drum)
 * Band 1 low-mid: bins  7–25  → 219– 781 Hz
 * Band 2 mid:     bins 26–63  → 813–1969 Hz
 * Band 3 high:    bins 64–191 → 2.0– 6.0 kHz */
static const uint16_t band_bin_start[NUM_BANDS] = {  1,  7, 26,  64 };
static const uint16_t band_bin_end[NUM_BANDS]   = {  6, 25, 63, 191 };

/* All buffers are file-scope static to avoid pressure on the DSP thread stack. */
static float32_t s_fft_input[NUM_FFT_SAMPLES];
static float32_t s_fft_output[NUM_FFT_SAMPLES];
/* magnitude[i] holds power for FFT bin i+1 (bin 0 = DC is skipped) */
static float32_t s_magnitude[NUM_FFT_SAMPLES / 2];
static float32_t s_hann_window[NUM_FFT_SAMPLES];

static arm_rfft_fast_instance_f32 s_rfft_inst;

static float32_t s_band_history[NUM_BANDS][HISTORY_LEN];
static uint8_t   s_history_idx;
static uint8_t   s_refractory[NUM_BANDS];

void audio_dsp_init(void)
{
	/* Length-specific initializer lets the linker discard twiddle tables
	 * for unused FFT sizes, saving ~3 KB flash vs. the generic init. */
	arm_rfft_fast_init_512_f32(&s_rfft_inst);
	arm_hanning_f32(s_hann_window, NUM_FFT_SAMPLES);
}

void audio_dsp_process(const int16_t *pcm, uint32_t seq,
		       struct audio_analysis_result *out)
{
	/* 1. Convert int16 → float32 and apply Hann window. */
	for (int i = 0; i < NUM_FFT_SAMPLES; i++) {
		s_fft_input[i] = (float32_t)pcm[i] * s_hann_window[i];
	}

#if defined(CONFIG_DEBUG) && defined(CONFIG_CPU_CORTEX_M)
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
	uint32_t t0 = DWT->CYCCNT;
#endif

	/* 2. 512-pt real FFT → packed Nyquist format in s_fft_output.
	 *    Output layout: [Re(DC) Re(Nyq)] [Re(1) Im(1)] ... [Re(N/2-1) Im(N/2-1)] */
	arm_rfft_fast_f32(&s_rfft_inst, s_fft_input, s_fft_output, 0 /* forward */);

	/* 3. Power spectrum for bins 1..(N/2-1); skip the packed DC/Nyquist pair at [0].
	 *    arm_cmplx_mag_squared_f32 operates on interleaved Re/Im pairs. */
	arm_cmplx_mag_squared_f32(&s_fft_output[2], s_magnitude, NUM_FFT_SAMPLES / 2 - 1);

#if defined(CONFIG_DEBUG) && defined(CONFIG_CPU_CORTEX_M)
	uint32_t cycles = DWT->CYCCNT - t0;
	LOG_DBG("FFT cycles: %u (~%u us)", cycles, cycles / 128);
#endif

	/* 4. Aggregate into sub-bands and run per-band beat detection. */
	out->seq = seq;

	for (int b = 0; b < NUM_BANDS; b++) {
		float32_t energy = 0.0f;
		for (int k = band_bin_start[b]; k <= band_bin_end[b]; k++) {
			energy += s_magnitude[k - 1]; /* s_magnitude[0] = bin 1 */
		}
		energy /= (float32_t)(band_bin_end[b] - band_bin_start[b] + 1);
		out->band_energy[b] = energy;

		s_band_history[b][s_history_idx] = energy;

		float32_t mean, sigma;
		arm_mean_f32(s_band_history[b], HISTORY_LEN, &mean);
		arm_std_f32(s_band_history[b], HISTORY_LEN, &sigma);

		bool beat = false;
		if (s_refractory[b] == 0 && energy > mean + BEAT_ALPHA * sigma) {
			beat = true;
			s_refractory[b] = BEAT_REFRACTORY;
		} else if (s_refractory[b] > 0) {
			s_refractory[b]--;
		}
		out->beat[b] = beat;
	}

	s_history_idx = (s_history_idx + 1) % HISTORY_LEN;
}
