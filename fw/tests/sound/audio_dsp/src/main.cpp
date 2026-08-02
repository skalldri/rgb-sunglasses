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
    float getSfDelta() override { return 0.10f; }
    void setSfDelta(float) override {}
    uint32_t getThresholdMode() override { return AUDIO_THRESHOLD_MODE_MEAN_SIGMA; }
    void setThresholdMode(uint32_t) override {}
};

/* Mutable provider for the Phase 3 threshold-shape tests: lets a test flip
 * mode/delta/alpha mid-stream, which is exactly what "sound dsp set mode 1"
 * does on hardware. */
class TunableAudioDspConfigProvider : public AudioDspConfigProvider {
   public:
    float getFluxGamma() override { return gamma_; }
    void setFluxGamma(float v) override { gamma_ = v; }
    float getBeatFluxFloor() override { return floor_; }
    void setBeatFluxFloor(float v) override { floor_ = v; }
    float getBeatAlpha() override { return alpha_; }
    void setBeatAlpha(float v) override { alpha_ = v; }
    uint32_t getBeatRefractoryFrames() override { return refractory_; }
    void setBeatRefractoryFrames(uint32_t v) override { refractory_ = v; }
    float getSfDelta() override { return sfDelta_; }
    void setSfDelta(float v) override { sfDelta_ = v; }
    uint32_t getThresholdMode() override { return mode_; }
    void setThresholdMode(uint32_t v) override { mode_ = v; }

    float gamma_ = 1000.0f;
    float floor_ = 0.005f;
    float alpha_ = 3.5f;
    uint32_t refractory_ = 5;
    float sfDelta_ = 0.10f;
    uint32_t mode_ = AUDIO_THRESHOLD_MODE_MEAN_SIGMA;
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

    p->setSfDelta(0.25f);
    zassert_within(p->getSfDelta(), 0.25f, 1e-6f, "sf_delta round-trip");
    p->setSfDelta(-1.0f); /* below range → clamps to 0 */
    zassert_within(p->getSfDelta(), 0.0f, 1e-6f, "sf_delta clamps low");
    p->setSfDelta(5.0f); /* above range → clamps to 2.0 */
    zassert_within(p->getSfDelta(), 2.0f, 1e-6f, "sf_delta clamps high");

    p->setThresholdMode(AUDIO_THRESHOLD_MODE_MEDIAN_DELTA);
    zassert_equal(p->getThresholdMode(), AUDIO_THRESHOLD_MODE_MEDIAN_DELTA, "mode round-trip");
    p->setThresholdMode(7); /* above range → clamps to 1 */
    zassert_equal(p->getThresholdMode(), AUDIO_THRESHOLD_MODE_MEDIAN_DELTA, "mode clamps high");

    /* Restore the documented defaults. */
    p->setFluxGamma(1000.0f);
    p->setBeatFluxFloor(0.005f);
    p->setBeatAlpha(0.3f);
    p->setBeatRefractoryFrames(5);
    p->setSfDelta(0.10f);
    p->setThresholdMode(AUDIO_THRESHOLD_MODE_MEAN_SIGMA);
}

/* Feed one "beat" — a loud 100 Hz burst frame followed by `gap - 1` silent
 * frames — through the detector, returning true if band 0 fired on the burst.
 * 16 frames at 32 ms is ~117 BPM, i.e. a plausible dance-music tempo. */
static bool run_one_burst(int16_t *tone, int16_t *silence, uint32_t *seq, int gap) {
    struct audio_analysis_result result;
    audio_dsp_process(tone, (*seq)++, &result);
    bool fired = result.beat[0];
    for (int i = 1; i < gap; i++) {
        audio_dsp_process(silence, (*seq)++, &result);
    }
    return fired;
}

/* ── Phase 3, Test A: threshold shape on a steady beat train ────────────────
 * THE motivating case for issue #264's Phase 3. On steady music the detected
 * beats are themselves inside the 1 s flux history, so they inflate sigma and
 * push mean+alpha*sigma above the next beat — the detector goes quiet exactly
 * when the music is most regular (hardware-measured: 7 fires per 60 s at
 * alpha=3.5). The median is unaffected by those outliers, so mode 1 keeps
 * firing.
 *
 * This test pins BOTH halves: mode 1 fires on essentially every burst after
 * the history fills, and mode 0 at the compiled default alpha=3.5 does not —
 * documenting the failure the way test_gain_compensation_misordered_is_harmful
 * pins the ordering hazard. */
ZTEST(audio_dsp, test_median_mode_survives_steady_beat_train) {
    /* Onset spacing is what drives sigma inflation, not tempo per se: with N
     * onsets of flux f inside the L-frame history, mode 0 mutes once
     * N/L + alpha*sqrt(p*(1-p)) > 1 (p = N/L). At kGap=8 the 32-frame history
     * holds 4 onsets, giving 1.28f > f — comfortably muted. (A 16-frame gap
     * puts a single-frame impulse train at 0.91f, right at the edge; real
     * captures clear it because onsets there are neither isolated nor
     * equal-amplitude. Using the unambiguous spacing keeps this test about the
     * threshold statistic rather than about how lifelike the stimulus is.)
     * 8 frames = 256 ms, i.e. eighth notes at ~117 BPM. */
    const int kGap = 8;
    const int kBursts = 16;         /* 16 * 8 = 128 frames = 4 history lengths */
    const int kWarmup = 4;          /* bursts ignored while the history fills */
    int16_t tone[AUDIO_FFT_SIZE], silence[AUDIO_FFT_SIZE];
    make_100hz_sine(tone);
    make_silence(silence);

    TunableAudioDspConfigProvider provider;
    audio_dsp_set_config_provider(&provider);

    /* --- mode 0 (mean + alpha*sigma) at the compiled default alpha --- */
    provider.mode_ = AUDIO_THRESHOLD_MODE_MEAN_SIGMA;
    provider.alpha_ = 3.5f;
    audio_dsp_init();
    uint32_t seq = 0;
    int mode0_fires = 0;
    for (int n = 0; n < kBursts; n++) {
        bool fired = run_one_burst(tone, silence, &seq, kGap);
        if (n >= kWarmup && fired) {
            mode0_fires++;
        }
    }

    /* --- mode 1 (median + delta), same stimulus --- */
    provider.mode_ = AUDIO_THRESHOLD_MODE_MEDIAN_DELTA;
    provider.sfDelta_ = 0.10f;
    audio_dsp_init();
    seq = 0;
    int mode1_fires = 0;
    for (int n = 0; n < kBursts; n++) {
        bool fired = run_one_burst(tone, silence, &seq, kGap);
        if (n >= kWarmup && fired) {
            mode1_fires++;
        }
    }

    audio_dsp_set_config_provider(NULL);

    const int scored = kBursts - kWarmup;
    zassert_equal(mode1_fires, scored,
                  "median mode should fire on every burst after warm-up: %d/%d", mode1_fires,
                  scored);
    zassert_true(mode0_fires < mode1_fires,
                 "mean+alpha*sigma should be muted by its own beats (sigma inflation): "
                 "mode0=%d mode1=%d of %d",
                 mode0_fires, mode1_fires, scored);
}

/* ── Phase 3, Test B: the mode switch takes effect mid-stream ───────────────
 * "sound dsp set mode 1" / a BLE write must change behavior without a restart,
 * since the whole point of the runtime switch is a no-reflash A/B on hardware.
 * Runs a beat train until mode 0 has gone quiet, then flips to mode 1 with the
 * SAME detector state and asserts firing resumes. */
ZTEST(audio_dsp, test_threshold_mode_switch_applies_mid_stream) {
    const int kGap = 8; /* same spacing rationale as the beat-train test above */
    int16_t tone[AUDIO_FFT_SIZE], silence[AUDIO_FFT_SIZE];
    make_100hz_sine(tone);
    make_silence(silence);

    TunableAudioDspConfigProvider provider;
    provider.mode_ = AUDIO_THRESHOLD_MODE_MEAN_SIGMA;
    provider.alpha_ = 3.5f;
    audio_dsp_set_config_provider(&provider);
    audio_dsp_init();

    uint32_t seq = 0;
    for (int n = 0; n < 4; n++) {
        run_one_burst(tone, silence, &seq, kGap);
    }
    bool fired_before_switch = run_one_burst(tone, silence, &seq, kGap);

    /* Flip the mode with no reset — same history, different threshold shape. */
    provider.mode_ = AUDIO_THRESHOLD_MODE_MEDIAN_DELTA;
    provider.sfDelta_ = 0.10f;
    bool fired_after_switch = run_one_burst(tone, silence, &seq, kGap);

    audio_dsp_set_config_provider(NULL);

    zassert_false(fired_before_switch, "mode 0 should be muted by this point in the train");
    zassert_true(fired_after_switch, "flipping to mode 1 must restore firing without a reset");
}

/* ── Phase 3, Test C: median value is the documented order statistic ────────
 * HISTORY_LEN is even, so "the median" needs a convention. audio_dsp.cpp takes
 * the UPPER middle (index HISTORY_LEN/2 of the sorted history) rather than
 * averaging the two middles. Pin that: with a history of 16 zeros and 16
 * distinct positive values, the upper middle is the SMALLEST positive one,
 * whereas the lower middle would be 0 and mean-of-middles would be half.
 *
 * Constructed via the threshold the detector reports (band_mean carries the
 * median in mode 1), which is also the only way to observe it from outside. */
ZTEST(audio_dsp, test_median_uses_upper_middle_of_even_history) {
    int16_t silence[AUDIO_FFT_SIZE], tone[AUDIO_FFT_SIZE];
    make_silence(silence);
    make_100hz_sine(tone);

    TunableAudioDspConfigProvider provider;
    provider.mode_ = AUDIO_THRESHOLD_MODE_MEDIAN_DELTA;
    provider.sfDelta_ = 0.0f; /* threshold == median exactly */
    audio_dsp_set_config_provider(&provider);
    audio_dsp_init();

    struct audio_analysis_result result;
    uint32_t seq = 0;

    /* Fill the whole history with zero-flux frames (steady silence). */
    for (int i = 0; i < HISTORY_LEN; i++) {
        audio_dsp_process(silence, seq++, &result);
    }
    zassert_within(result.band_mean[0], 0.0f, 1e-9f,
                   "median of an all-zero history must be 0, got %f",
                   (double)result.band_mean[0]);

    /* Now push in HISTORY_LEN/2 positive-flux frames. Alternating tone/silence
     * yields a positive flux on each tone frame and zero on each silence frame
     * (half-wave rectification), so the ring ends up half positive, half zero
     * — the exact even-split case the convention governs. */
    for (int i = 0; i < HISTORY_LEN; i++) {
        audio_dsp_process((i % 2 == 0) ? tone : silence, seq++, &result);
    }

    /* Upper middle of {16 zeros, 16 positives} is the smallest positive value,
     * so the reported median must be strictly positive. The lower-middle
     * convention would report exactly 0 here. */
    zassert_true(result.band_mean[0] > 0.0f,
                 "upper-middle median of a half-positive history must be > 0, got %f",
                 (double)result.band_mean[0]);

    audio_dsp_set_config_provider(NULL);
}

/* ── Phase 3, Test D: silence stays silent in mode 1 ────────────────────────
 * With an all-zero history the median is 0, so the adaptive threshold collapses
 * to sfDelta alone — the flux floor is what has to keep silence quiet. Mirrors
 * test_silence_no_beat for the new mode. */
ZTEST(audio_dsp, test_median_mode_silence_no_beat) {
    int16_t silence[AUDIO_FFT_SIZE];
    make_silence(silence);

    TunableAudioDspConfigProvider provider;
    provider.mode_ = AUDIO_THRESHOLD_MODE_MEDIAN_DELTA;
    provider.sfDelta_ = 0.0f; /* worst case: threshold is the bare median */
    audio_dsp_set_config_provider(&provider);
    audio_dsp_init();

    struct audio_analysis_result result;
    for (int i = 0; i < HISTORY_LEN * 3; i++) {
        audio_dsp_process(silence, i, &result);
        for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
            zassert_false(result.beat[b], "silence fired a beat in band %d at frame %d", b, i);
        }
    }

    audio_dsp_set_config_provider(NULL);
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
    /* Alpha is pinned rather than inherited from the default provider: this
     * test is about the COMPENSATION contract, and its stimulus is calibrated
     * against a mean+3.5*sigma threshold (see the flux-magnitude note below).
     * Phase 3 retuned the shipped default to 0.3, at which the history-building
     * bumps clear the threshold themselves and leave the per-band refractory
     * counter non-zero on the final onset frame — the assertion would then fail
     * for a reason that has nothing to do with gain compensation. */
    TunableAudioDspConfigProvider provider;
    provider.alpha_ = 3.5f;
    provider.mode_ = AUDIO_THRESHOLD_MODE_MEAN_SIGMA;
    audio_dsp_set_config_provider(&provider);
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
    bool onset_fired = result.beat[0];

    audio_dsp_set_config_provider(NULL);
    zassert_true(onset_fired, "real onset right after a compensated step must fire");
}

/* ── Test 10: Compensation is exact for quiet signals — discriminating form ──
 * The previous-frame state is LINEAR energy, so the correction is an exact
 * multiply at any level — including γE ≪ 1 where a log-domain offset (adding
 * ln(10^(0.05·steps)) ≈ ±0.115 to the stored log) would be wrong. A steady
 * tone can't discriminate the two (half-wave rectification hides the error in
 * both directions), so this test crosses a LEVEL CHANGE and a gain step
 * together and asserts the flux against its computed exact value: at
 * γE ≈ 0.05→0.16 the exact form gives ~0.105 while the log-offset form gives
 * ~0.215 — an order of magnitude beyond the tolerance below. */
ZTEST(audio_dsp, test_gain_compensation_exact_when_quiet) {
    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;
    uint32_t seq = 0;

    make_tone(pcm, 300.0, 1.0f); /* γ·E lands around 0.05 for band 0 */
    for (int i = 0; i < 8; i++) {
        audio_dsp_process(pcm, seq++, &result);
    }
    float e_a = result.band_energy[0];

    /* Gain steps down; the next block is a LOUDER tone captured at the new
     * gain. Its flux must equal the pure musical change, with the gain step
     * fully cancelled. */
    audio_dsp_compensate_gain_change(-1);
    make_tone(pcm, 600.0, STEP_DOWN_AMP);
    audio_dsp_process(pcm, seq++, &result);
    float e_b = result.band_energy[0];

    const float gamma = 1000.0f; /* default provider's fluxGamma */
    const float power_down = 0.8912509f; /* 10^-0.05, one step in power */
    float expected = log1pf(gamma * e_b) - log1pf(gamma * e_a * power_down);
    zassert_true(expected > 0.05f, "test setup must produce meaningful flux (%f)",
                 (double)expected);
    zassert_within(result.band_flux[0], expected, 0.01f,
                   "flux must match the exact-compensation value (got %f, expected %f)",
                   (double)result.band_flux[0], (double)expected);
}

/* ── Test 10b: The full production sequence around an AGC step is clean ──────
 * Mirrors the fixed audio_dsp_thread_func() ordering exactly: the block
 * captured at the old gain is PROCESSED FIRST, then the gain step +
 * compensation land, then blocks captured at the new gain arrive. No frame in
 * the sequence may fire a beat or show non-trivial flux. */
ZTEST(audio_dsp, test_gain_step_production_sequence) {
    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;
    uint32_t seq = 0;

    make_tone(pcm, 8000.0, 1.0f);
    for (int i = 0; i < HISTORY_LEN; i++) {
        audio_dsp_process(pcm, seq++, &result);
    }

    /* Decision frame: processed at the old gain, THEN the step is applied. */
    audio_dsp_process(pcm, seq++, &result);
    zassert_false(result.beat[0]);
    audio_dsp_compensate_gain_change(-1);

    /* Post-step frames arrive in the new gain domain. */
    make_tone(pcm, 8000.0, STEP_DOWN_AMP);
    for (int i = 0; i < 3; i++) {
        audio_dsp_process(pcm, seq++, &result);
        zassert_false(result.beat[0], "no beat may fire around a compensated step (frame %d)",
                      i);
        zassert_true(result.band_flux[0] < 0.01f,
                     "flux around a compensated step must be ~0 (frame %d: %f)", i,
                     (double)result.band_flux[0]);
    }
}

/* ── Test 10c: The misordering hazard is real (contract documentation) ───────
 * Compensating BEFORE the last old-gain block is processed — the pre-fix
 * firmware ordering — produces a false flux of ln(10^0.05) ≈ 0.115 on that
 * frame. This test pins the hazard the ordering contract in audio_dsp.h guards
 * against; if it ever stops failing-the-old-way, the contract text is stale. */
ZTEST(audio_dsp, test_gain_compensation_misordered_is_harmful) {
    audio_dsp_init();

    int16_t pcm[AUDIO_FFT_SIZE];
    struct audio_analysis_result result;
    uint32_t seq = 0;

    make_tone(pcm, 8000.0, 1.0f); /* loud: γE ≫ 1, log-domain shift ≈ exact */
    for (int i = 0; i < 8; i++) {
        audio_dsp_process(pcm, seq++, &result);
    }

    /* WRONG order: compensate, then process a block still at the OLD gain. */
    audio_dsp_compensate_gain_change(-1);
    audio_dsp_process(pcm, seq++, &result);

    zassert_within(result.band_flux[0], 0.1151f, 0.02f,
                   "misordered compensation must inject ~ln(10^0.05) of false flux (got %f)",
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
