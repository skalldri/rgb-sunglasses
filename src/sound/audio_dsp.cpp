/* Include all Zephyr headers before arm_math.h to avoid ROUND_UP/ROUND_DOWN
 * macro collision (Zephyr issue #64327). */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <arm_math.h>

#include "audio_dsp.h"

LOG_MODULE_REGISTER(audio_dsp);

#define NUM_FFT_SAMPLES    AUDIO_FFT_SIZE
#define NUM_BANDS          AUDIO_NUM_BANDS
#define NUM_DISPLAY_BUCKETS AUDIO_NUM_DISPLAY_BUCKETS
#define HISTORY_LEN     32    /* ~1 s at 32 ms/frame */
/* threshold = mean + alpha * sigma.  At 2.0 anything above the 97.7th
 * percentile fires (~1 false per few seconds from ambient noise at 31 Hz).
 * 3.5 requires a genuine outlier spike; raise toward 4.0 in noisy rooms. */
#define BEAT_ALPHA      3.5f
#define BEAT_REFRACTORY 5     /* minimum frames between beats per band */
/* Absolute normalised power floor.  Bands 1–3 have much lower raw energy
 * than band 0; without this floor their tiny absolute fluctuations pass the
 * relative threshold even when nothing is happening.  1e-3 is safely above
 * the measured ambient for all bands while remaining well below a real beat. */
#define BEAT_ENERGY_FLOOR 1e-3f

/* Sub-band bin boundaries (512-pt FFT at 16 kHz, bin width = 31.25 Hz).
 * Band 0 bass:    bins  1– 6  →  31– 200 Hz (kick drum)
 * Band 1 low-mid: bins  7–25  → 219– 781 Hz
 * Band 2 mid:     bins 26–63  → 813–1969 Hz
 * Band 3 high:    bins 64–191 → 2.0– 6.0 kHz */
static const uint16_t band_bin_start[NUM_BANDS] = {  1,  7, 26,  64 };
static const uint16_t band_bin_end[NUM_BANDS]   = {  6, 25, 63, 191 };

/* Display bucket boundaries modelled on a professional VU meter, capped at 3 kHz.
 * Each previous 10-bucket range is split in two at a natural frequency boundary.
 * (bin width = 31.25 Hz; bin 1 = 31 Hz is sub-bass below PDM mic range, skipped)
 *
 * Bucket  0:  bins   2–  5   62– 156 Hz  (low bass)
 * Bucket  1:  bins   6–  9  188– 281 Hz  (upper bass)
 * Bucket  2:  bins  10– 11  313– 344 Hz  (VU 300–450 lower half)
 * Bucket  3:  bins  12– 14  375– 438 Hz  (VU 300–450 upper half)
 * Bucket  4:  bins  15– 16  469– 500 Hz  (VU 450–600 lower half)
 * Bucket  5:  bins  17– 19  531– 594 Hz  (VU 450–600 upper half)
 * Bucket  6:  bins  20– 24  625– 750 Hz  (VU 600–750)
 * Bucket  7:  bins  25– 28  781– 875 Hz  (VU 750–900)
 * Bucket  8:  bins  29– 32  906–1000 Hz  (VU 900–1000)
 * Bucket  9:  bins  33– 38 1031–1188 Hz  (VU 1000–1200)
 * Bucket 10:  bins  39– 44 1219–1375 Hz  (VU 1200–1400)
 * Bucket 11:  bins  45– 48 1406–1500 Hz  (VU 1400–1500)
 * Bucket 12:  bins  49– 54 1531–1688 Hz  (VU 1500–1700)
 * Bucket 13:  bins  55– 57 1719–1781 Hz  (VU 1700–1800)
 * Bucket 14:  bins  58– 60 1813–1875 Hz  (VU 1800–2000 lower half)
 * Bucket 15:  bins  61– 64 1906–2000 Hz  (VU 1800–2000 upper half)
 * Bucket 16:  bins  65– 72 2031–2250 Hz  (VU 2000–2500 lower half)
 * Bucket 17:  bins  73– 80 2281–2500 Hz  (VU 2000–2500 upper half)
 * Bucket 18:  bins  81– 88 2531–2750 Hz  (VU 2500–3000 lower half)
 * Bucket 19:  bins  89– 96 2781–3000 Hz  (VU 2500–3000 upper half) */
static const uint16_t display_bucket_start[NUM_DISPLAY_BUCKETS] = {
	 2,  6, 10, 12, 15, 17, 20, 25, 29, 33, 39, 45, 49, 55, 58, 61, 65, 73, 81, 89
};
static const uint16_t display_bucket_end[NUM_DISPLAY_BUCKETS] = {
	 5,  9, 11, 14, 16, 19, 24, 28, 32, 38, 44, 48, 54, 57, 60, 64, 72, 80, 88, 96
};

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
	/* 1. Normalise int16 to [-1, 1] and apply Hann window. */
	for (int i = 0; i < NUM_FFT_SAMPLES; i++) {
		s_fft_input[i] = ((float32_t)pcm[i] / 32768.0f) * s_hann_window[i];
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
		out->band_mean[b]  = mean;
		out->band_sigma[b] = sigma;

		bool beat = false;
		if (s_refractory[b] == 0 && energy > BEAT_ENERGY_FLOOR &&
		    energy > mean + BEAT_ALPHA * sigma) {
			beat = true;
			s_refractory[b] = BEAT_REFRACTORY;
		} else if (s_refractory[b] > 0) {
			s_refractory[b]--;
		}
		out->beat[b] = beat;
	}

	s_history_idx = (s_history_idx + 1) % HISTORY_LEN;

	/* 5. Compute mean power for each display bucket. */
	for (int b = 0; b < NUM_DISPLAY_BUCKETS; b++) {
		float32_t energy = 0.0f;
		for (int k = display_bucket_start[b]; k <= display_bucket_end[b]; k++) {
			energy += s_magnitude[k - 1]; /* s_magnitude[0] = bin 1 */
		}
		energy /= (float32_t)(display_bucket_end[b] - display_bucket_start[b] + 1);
		out->display_bucket_energy[b] = energy;
	}
}
