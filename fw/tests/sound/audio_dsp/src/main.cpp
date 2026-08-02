#include <math.h>
#include <zephyr/ztest.h>

#include "audio_dsp.h"

/* Re-expose internal constants so tests can reason about history depth. */
#define HISTORY_LEN 32
#define BEAT_REFRACTORY 5

/* Build a 100 Hz full-amplitude sine wave into buf (512 int16 samples at 16 kHz).
 * 100 Hz sits in band 0 (bins 3–4 at 31.25 Hz/bin). */
static void make_100hz_sine(int16_t *buf) {
    for (int i = 0; i < AUDIO_FFT_SIZE; i++) {
        double t = (double)i / 16000.0;
        buf[i] = (int16_t)(32767.0 * sin(2.0 * M_PI * 100.0 * t));
    }
}

static void make_silence(int16_t *buf) {
    memset(buf, 0, AUDIO_FFT_SIZE * sizeof(int16_t));
}

/* ── Test 1: Band energy localisation ───────────────────────────────────────
 * A 100 Hz sine should concentrate energy in band 0 (bass) and produce
 * negligibly less energy in band 2 (mid) and band 3 (high). */
ZTEST(audio_dsp, test_bass_sine_band_energy) {
    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;

    make_100hz_sine(pcm);
    audio_dsp_process(pcm, 0, &result);

    zassert_true(result.band_energy[0] > result.band_energy[2],
                 "bass energy (%f) should exceed mid energy (%f)", (double)result.band_energy[0],
                 (double)result.band_energy[2]);
    zassert_true(result.band_energy[0] > result.band_energy[3],
                 "bass energy (%f) should exceed high energy (%f)", (double)result.band_energy[0],
                 (double)result.band_energy[3]);
}

/* ── Test 2: Beat detection fires on onset (Level 3 spectral flux) ───────────
 * Strategy: fill history with silence so flux_mean and flux_sigma are near zero.
 * Then inject a loud 100 Hz sine.  The FIRST frame produces a large positive
 * flux (log_e - log_e_prev ≈ log1p(GAMMA * loud_energy)), which spikes far above
 * the near-zero adaptive threshold → beat fires. */
ZTEST(audio_dsp, test_beat_fires_on_energy_step) {
    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;

    /* Fill history with silence. */
    make_silence(pcm);
    for (int i = 0; i < HISTORY_LEN; i++) {
        audio_dsp_process(pcm, i, &result);
    }

    /* Inject a loud bass tone — onset flux spike should trigger beat[0]. */
    make_100hz_sine(pcm);
    bool beat_fired = false;
    /* Run enough frames to clear any residual refractory state and catch a fire. */
    for (int i = 0; i < BEAT_REFRACTORY + 2; i++) {
        audio_dsp_process(pcm, HISTORY_LEN + i, &result);
        if (result.beat[0]) {
            beat_fired = true;
        }
    }

    zassert_true(beat_fired, "beat[0] should fire when loud bass onset follows silent history");
}

/* ── Test 3: No beats on silence ────────────────────────────────────────────
 * Feeding zeros for many frames must never produce a beat. */
ZTEST(audio_dsp, test_silence_no_beat) {
    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;

    make_silence(pcm);
    for (int i = 0; i < HISTORY_LEN * 2; i++) {
        audio_dsp_process(pcm, i, &result);
        for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
            zassert_false(result.beat[b], "beat[%d] should not fire on silence (frame %d)", b, i);
        }
    }
}

/* ── Test 4: Display bucket energy localisation ──────────────────────────────
 * A 100 Hz sine (bin ~3) sits inside bucket 0 (bins 2–5, 62–156 Hz).
 * Its energy must exceed bucket 12 (1531–1688 Hz) and bucket 19 (2781–3000 Hz). */
ZTEST(audio_dsp, test_100hz_sine_localises_in_low_display_bucket) {
    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;

    make_100hz_sine(pcm);
    audio_dsp_process(pcm, 0, &result);

    zassert_true(result.display_bucket_energy[0] > result.display_bucket_energy[12],
                 "100 Hz energy in bucket 0 (%f) should exceed bucket 12 (%f)",
                 (double)result.display_bucket_energy[0], (double)result.display_bucket_energy[12]);
    zassert_true(result.display_bucket_energy[0] > result.display_bucket_energy[19],
                 "100 Hz energy in bucket 0 (%f) should exceed bucket 19 (%f)",
                 (double)result.display_bucket_energy[0], (double)result.display_bucket_energy[19]);
}

/* ── Test 5: Sustained tone fires beat at most once (Level 3 key property) ──
 * This is the defining advantage of spectral flux over Level 2 (raw energy):
 * flux = max(0, log_e_now - log_e_prev) drops to ~0 once energy stabilises,
 * so no further beats fire on a steady-state tone. */
ZTEST(audio_dsp, test_sustained_tone_fires_beat_at_most_once) {
    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;

    /* Establish silent history → near-zero flux mean and sigma. */
    make_silence(pcm);
    for (int i = 0; i < HISTORY_LEN; i++) {
        audio_dsp_process(pcm, i, &result);
    }

    /* Run a constant-amplitude 100 Hz tone for many frames.
     * The onset (first frame) may trigger one beat; subsequent frames
     * produce zero flux and must not trigger further beats. */
    make_100hz_sine(pcm);
    int beats_fired = 0;
    for (int i = 0; i < HISTORY_LEN; i++) {
        audio_dsp_process(pcm, HISTORY_LEN + i, &result);
        if (result.beat[0]) {
            beats_fired++;
        }
    }

    zassert_true(beats_fired <= 1,
                 "sustained tone should trigger beat at most once; fired %d times", beats_fired);
}

/* ── Test 6: Injected AudioDspConfigProvider actually changes behavior ──────
 * Mirrors test_beat_fires_on_energy_step's scenario (silent history, then a loud bass
 * onset), but with an absurdly high BeatAlpha injected via audio_dsp_set_config_provider().
 * The same onset that reliably fires a beat with default parameters must NOT fire one
 * here, proving audio_dsp_process() actually reads from the injected provider rather
 * than the compiled-in defaults. Resets to the default provider (nullptr) at the end so
 * this doesn't leak into any other test in this suite. */
namespace {
class FakeAudioDspConfigProvider : public AudioDspConfigProvider {
   public:
    float getFluxGamma() override { return 1000.0f; }
    void setFluxGamma(float) override {}
    float getBeatFluxFloor() override { return 0.005f; }
    void setBeatFluxFloor(float) override {}
    float getBeatAlpha() override { return 1000.0f; /* practically unreachable threshold */ }
    void setBeatAlpha(float) override {}
    uint32_t getBeatRefractoryFrames() override { return 5; }
    void setBeatRefractoryFrames(uint32_t) override {}
};
}  // namespace

ZTEST(audio_dsp, test_custom_config_provider_overrides_defaults) {
    FakeAudioDspConfigProvider fakeProvider;
    audio_dsp_set_config_provider(&fakeProvider);

    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;

    make_silence(pcm);
    for (int i = 0; i < HISTORY_LEN; i++) {
        audio_dsp_process(pcm, i, &result);
    }

    make_100hz_sine(pcm);
    bool beat_fired = false;
    for (int i = 0; i < BEAT_REFRACTORY + 2; i++) {
        audio_dsp_process(pcm, HISTORY_LEN + i, &result);
        if (result.beat[0]) {
            beat_fired = true;
        }
    }

    audio_dsp_set_config_provider(NULL);

    zassert_false(beat_fired, "beat[0] should not fire with an injected BeatAlpha=1000 threshold");
}

/* ── Test 7: band_flux is populated and consistent with the beat decision ────
 * band_flux (added for the issue #264 debugging environment) must carry the
 * actual per-frame ODF value: ~0 on silence, large and positive on the onset
 * frame — and on a beat frame it must exceed the mean + alpha*sigma threshold
 * the detector reports through band_mean/band_sigma. */
ZTEST(audio_dsp, test_band_flux_populated) {
    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;

    make_silence(pcm);
    for (int i = 0; i < HISTORY_LEN; i++) {
        audio_dsp_process(pcm, i, &result);
    }
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        zassert_true(result.band_flux[b] < 0.001f, "silence flux in band %d should be ~0 (%f)",
                     b, (double)result.band_flux[b]);
    }

    make_100hz_sine(pcm);
    audio_dsp_process(pcm, HISTORY_LEN, &result);

    zassert_true(result.beat[0], "onset should fire beat[0]");
    zassert_true(result.band_flux[0] > 1.0f, "onset flux should be large (%f)",
                 (double)result.band_flux[0]);
    /* The reported flux/mean/sigma triple must reproduce the fire decision
     * (default alpha = 3.5). */
    zassert_true(result.band_flux[0] > result.band_mean[0] + 3.5f * result.band_sigma[0],
                 "flux (%f) should exceed reported threshold (%f)",
                 (double)result.band_flux[0],
                 (double)(result.band_mean[0] + 3.5f * result.band_sigma[0]));
}

/* ── Test 8: Default provider setters clamp and round-trip ──────────────────
 * The "sound dsp set" shell path writes through audio_dsp_get_config_provider();
 * with no BT provider injected that's the built-in DefaultAudioDspConfigProvider.
 * Verify set→get round-trips and that out-of-range values clamp to the same
 * ranges the BT-backed AudioConfig enforces. Restores defaults at the end so
 * other tests in this suite are unaffected regardless of execution order. */
ZTEST(audio_dsp, test_default_provider_setters_clamp) {
    audio_dsp_set_config_provider(NULL); /* ensure the built-in default provider */
    AudioDspConfigProvider *p = audio_dsp_get_config_provider();
    zassert_not_null(p, "default provider must never be NULL");

    p->setFluxGamma(500.0f);
    zassert_within(p->getFluxGamma(), 500.0f, 1e-3f, "gamma round-trip");
    p->setFluxGamma(0.001f); /* below range → clamps to 1.0 */
    zassert_within(p->getFluxGamma(), 1.0f, 1e-6f, "gamma clamps low");

    p->setBeatFluxFloor(0.02f);
    zassert_within(p->getBeatFluxFloor(), 0.02f, 1e-6f, "floor round-trip");
    p->setBeatFluxFloor(2.0f); /* above range → clamps to 1.0 */
    zassert_within(p->getBeatFluxFloor(), 1.0f, 1e-6f, "floor clamps high");

    p->setBeatAlpha(2.5f);
    zassert_within(p->getBeatAlpha(), 2.5f, 1e-6f, "alpha round-trip");
    p->setBeatAlpha(0.0f); /* below range → clamps to 0.1 */
    zassert_within(p->getBeatAlpha(), 0.1f, 1e-6f, "alpha clamps low");

    p->setBeatRefractoryFrames(10);
    zassert_equal(p->getBeatRefractoryFrames(), 10, "refractory round-trip");
    p->setBeatRefractoryFrames(999); /* above uint8_t counter range → clamps to 255 */
    zassert_equal(p->getBeatRefractoryFrames(), 255, "refractory clamps to 255");

    /* Restore the documented defaults. */
    p->setFluxGamma(1000.0f);
    p->setBeatFluxFloor(0.005f);
    p->setBeatAlpha(3.5f);
    p->setBeatRefractoryFrames(5);
}

/* Helper for the gain-compensation tests: 100 Hz sine at the given amplitude,
 * optionally pre-scaled by `scale` (simulating the hardware capturing at a
 * changed PDM gain). */
static void make_tone(int16_t *buf, double amplitude, float scale) {
    for (int i = 0; i < AUDIO_FFT_SIZE; i++) {
        double t = (double)i / 16000.0;
        buf[i] = (int16_t)((float)(amplitude * sin(2.0 * M_PI * 100.0 * t)) * scale);
    }
}

/* One PDM register step = 0.5 dB of amplitude. */
#define STEP_DOWN_AMP 0.9440609f /* 10^(-0.025) */

/* ── Test 9: Gain compensation carries detector state across a step ─────────
 * The pre-Phase-1 behavior reset ALL history on every AGC gain step, blinding
 * the adaptive threshold for ~1 s (hardware-measured: 16 steps in 30 s of
 * music → ~30% of frames blinded; issue #264). The fix scales the linear
 * previous-frame energy instead. Assert, across a simulated -1 step:
 *   (a) no spurious beat and ~zero flux on the step frame,
 *   (b) the threshold history SURVIVES (band_mean stays near its value),
 *   (c) a genuine onset right after the step still fires. */
ZTEST(audio_dsp, test_gain_compensation_preserves_history) {
    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;
    uint32_t seq = 0;

    /* Build non-trivial flux history: a small quiet→medium bump every 8 frames
     * (flux ≈ ln(4) per bump — small enough that a later real onset clears the
     * mean+3.5σ threshold). */
    for (int cycle = 0; cycle < 5; cycle++) {
        for (int i = 0; i < 7; i++) {
            make_tone(pcm, 2000.0, 1.0f);
            audio_dsp_process(pcm, seq++, &result);
        }
        make_tone(pcm, 4000.0, 1.0f);
        audio_dsp_process(pcm, seq++, &result);
    }
    /* Settle on quiet so the step frame is quiet-vs-quiet. */
    for (int i = 0; i < 3; i++) {
        make_tone(pcm, 2000.0, 1.0f);
        audio_dsp_process(pcm, seq++, &result);
    }
    float mean_before = result.band_mean[0];
    zassert_true(mean_before > 0.05f, "test needs non-trivial history (mean=%f)",
                 (double)mean_before);

    /* Simulated -1 gain step: hardware now captures everything 0.5 dB quieter. */
    audio_dsp_compensate_gain_change(-1);
    make_tone(pcm, 2000.0, STEP_DOWN_AMP);
    audio_dsp_process(pcm, seq++, &result);

    zassert_false(result.beat[0], "compensated gain step must not fire a beat");
    zassert_true(result.band_flux[0] < 0.01f, "flux across a compensated step should be ~0 (%f)",
                 (double)result.band_flux[0]);
    zassert_true(result.band_mean[0] > 0.6f * mean_before,
                 "threshold history must survive the step (mean %f -> %f)",
                 (double)mean_before, (double)result.band_mean[0]);

    /* A genuine onset (16x amplitude vs the quiet tone) in the new gain domain
     * must still fire — the detector is neither blinded nor desensitized. */
    make_tone(pcm, 32000.0, STEP_DOWN_AMP);
    audio_dsp_process(pcm, seq++, &result);
    zassert_true(result.beat[0], "real onset right after a compensated step must fire");
}

/* ── Test 10: Compensation is exact for quiet signals too ────────────────────
 * The previous-frame state is LINEAR energy, so the correction is an exact
 * multiply at any level — including γE ≪ 1 where a log-domain offset would be
 * wrong. A quiet tone across a compensated step must produce ~zero flux. */
ZTEST(audio_dsp, test_gain_compensation_exact_when_quiet) {
    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;
    uint32_t seq = 0;

    make_tone(pcm, 300.0, 1.0f); /* γ·E lands around 0.01-0.1 for band 0 */
    for (int i = 0; i < 8; i++) {
        audio_dsp_process(pcm, seq++, &result);
    }

    audio_dsp_compensate_gain_change(-1);
    make_tone(pcm, 300.0, STEP_DOWN_AMP);
    audio_dsp_process(pcm, seq++, &result);

    zassert_false(result.beat[0], "quiet compensated step must not fire");
    zassert_true(result.band_flux[0] < 0.005f,
                 "flux across a compensated step on a quiet tone should be ~0 (%f)",
                 (double)result.band_flux[0]);
}

/* ── Test 11: A large jump falls back to the full reset ─────────────────────
 * Manual "sound agc gain" changes can jump many steps at once; |steps| > 4
 * takes the audio_dsp_reset_history() path, which zeroes the threshold
 * statistics (observable as band_mean == band_sigma == 0 on the next frame). */
ZTEST(audio_dsp, test_gain_compensation_large_jump_resets) {
    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;
    uint32_t seq = 0;

    for (int cycle = 0; cycle < 3; cycle++) {
        make_tone(pcm, 2000.0, 1.0f);
        for (int i = 0; i < 7; i++) {
            audio_dsp_process(pcm, seq++, &result);
        }
        make_tone(pcm, 4000.0, 1.0f);
        audio_dsp_process(pcm, seq++, &result);
    }

    audio_dsp_compensate_gain_change(-10);
    make_tone(pcm, 2000.0, 1.0f);
    audio_dsp_process(pcm, seq++, &result);

    zassert_false(result.beat[0], "first frame after a reset must not fire");
    zassert_equal(result.band_mean[0], 0.0f, "reset must zero the flux mean");
    zassert_equal(result.band_sigma[0], 0.0f, "reset must zero the flux sigma");
}

ZTEST_SUITE(audio_dsp, NULL, NULL, NULL, NULL, NULL);
