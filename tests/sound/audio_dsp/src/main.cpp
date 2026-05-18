#include <zephyr/ztest.h>
#include <math.h>
#include "audio_dsp.h"

/* Re-expose internal constants so tests can reason about history depth. */
#define HISTORY_LEN     32
#define BEAT_REFRACTORY 5

/* Build a 100 Hz full-amplitude sine wave into buf (512 int16 samples at 16 kHz).
 * 100 Hz sits in band 0 (bins 3–4 at 31.25 Hz/bin). */
static void make_100hz_sine(int16_t *buf)
{
	for (int i = 0; i < AUDIO_FFT_SIZE; i++) {
		double t = (double)i / 16000.0;
		buf[i] = (int16_t)(32767.0 * sin(2.0 * M_PI * 100.0 * t));
	}
}

static void make_silence(int16_t *buf)
{
	memset(buf, 0, AUDIO_FFT_SIZE * sizeof(int16_t));
}

/* ── Test 1: Band energy localisation ───────────────────────────────────────
 * A 100 Hz sine should concentrate energy in band 0 (bass) and produce
 * negligibly less energy in band 2 (mid) and band 3 (high). */
ZTEST(audio_dsp, test_bass_sine_band_energy)
{
	audio_dsp_init();

	int16_t pcm[AUDIO_FFT_SIZE];
	struct audio_analysis_result result;

	make_100hz_sine(pcm);
	audio_dsp_process(pcm, 0, &result);

	zassert_true(result.band_energy[0] > result.band_energy[2],
		     "bass energy (%f) should exceed mid energy (%f)",
		     (double)result.band_energy[0], (double)result.band_energy[2]);
	zassert_true(result.band_energy[0] > result.band_energy[3],
		     "bass energy (%f) should exceed high energy (%f)",
		     (double)result.band_energy[0], (double)result.band_energy[3]);
}

/* ── Test 2: Beat detection fires on a step-change in energy ────────────────
 * Strategy: fill the history with silence so mean and sigma are near zero,
 * then inject a loud 100 Hz sine. The first loud frame should fire beat[0]
 * because energy >> mean + 2*sigma (sigma is near zero from silent history). */
ZTEST(audio_dsp, test_beat_fires_on_energy_step)
{
	audio_dsp_init();

	int16_t pcm[AUDIO_FFT_SIZE];
	struct audio_analysis_result result;

	/* Fill history with silence. */
	make_silence(pcm);
	for (int i = 0; i < HISTORY_LEN; i++) {
		audio_dsp_process(pcm, i, &result);
	}

	/* Inject a loud bass tone — should trigger beat[0]. */
	make_100hz_sine(pcm);
	bool beat_fired = false;
	/* Run enough frames to clear any residual refractory state and catch a fire. */
	for (int i = 0; i < BEAT_REFRACTORY + 2; i++) {
		audio_dsp_process(pcm, HISTORY_LEN + i, &result);
		if (result.beat[0]) {
			beat_fired = true;
		}
	}

	zassert_true(beat_fired,
		     "beat[0] should fire when loud bass follows silent history");
}

/* ── Test 3: No beats on silence ────────────────────────────────────────────
 * Feeding zeros for many frames must never produce a beat. */
ZTEST(audio_dsp, test_silence_no_beat)
{
	audio_dsp_init();

	int16_t pcm[AUDIO_FFT_SIZE];
	struct audio_analysis_result result;

	make_silence(pcm);
	for (int i = 0; i < HISTORY_LEN * 2; i++) {
		audio_dsp_process(pcm, i, &result);
		for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
			zassert_false(result.beat[b],
				      "beat[%d] should not fire on silence (frame %d)", b, i);
		}
	}
}

/* ── Test 4: Display bucket energy localisation ──────────────────────────────
 * A 100 Hz sine (bin ~3) sits inside bucket 0 (bins 2–5, 62–156 Hz).
 * Its energy must exceed bucket 12 (1531–1688 Hz) and bucket 19 (2781–3000 Hz). */
ZTEST(audio_dsp, test_100hz_sine_localises_in_low_display_bucket)
{
	audio_dsp_init();

	int16_t pcm[AUDIO_FFT_SIZE];
	struct audio_analysis_result result;

	make_100hz_sine(pcm);
	audio_dsp_process(pcm, 0, &result);

	zassert_true(result.display_bucket_energy[0] > result.display_bucket_energy[12],
		     "100 Hz energy in bucket 0 (%f) should exceed bucket 12 (%f)",
		     (double)result.display_bucket_energy[0],
		     (double)result.display_bucket_energy[12]);
	zassert_true(result.display_bucket_energy[0] > result.display_bucket_energy[19],
		     "100 Hz energy in bucket 0 (%f) should exceed bucket 19 (%f)",
		     (double)result.display_bucket_energy[0],
		     (double)result.display_bucket_energy[19]);
}

ZTEST_SUITE(audio_dsp, NULL, NULL, NULL, NULL, NULL);
