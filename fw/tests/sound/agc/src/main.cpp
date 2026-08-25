/* Unit tests for AgcController (fw/src/sound/agc_controller.cpp) — the real
 * firmware AGC policy, exercised deterministically frame by frame. */
#include <zephyr/ztest.h>

#include "agc_controller.h"
#include "audio_dsp.h" /* audio_dsp_gain_amplitude_ratio() for the input-referred test */

namespace {
/* Directly configurable provider; gate defaults to 0 so tests that aren't
 * about the noise gate can't trip it accidentally (silent = smoothed < gate,
 * and a freshly-zeroed RMS window smooths anything down toward 0). */
class FakeAgcConfig : public AgcConfigProvider {
   public:
    float tlow = 0.005f;
    float thigh = 0.008f;
    float gate = 0.0f;
    uint32_t rate = 1;
    uint32_t attack = 3;
    uint32_t release = 15;

    float getTargetLow() override { return tlow; }
    void setTargetLow(float v) override { tlow = v; }
    float getTargetHigh() override { return thigh; }
    void setTargetHigh(float v) override { thigh = v; }
    uint32_t getRateLimitFrames() override { return rate; }
    void setRateLimitFrames(uint32_t v) override { rate = v; }
    uint32_t getAttackFrames() override { return attack; }
    void setAttackFrames(uint32_t v) override { attack = v; }
    uint32_t getReleaseFrames() override { return release; }
    void setReleaseFrames(uint32_t v) override { release = v; }
    float getNoiseGateRms() override { return gate; }
    void setNoiseGateRms(float v) override { gate = v; }
};
}  // namespace

/* Attack: exactly attackFrames CONSECUTIVE over-targetHigh frames fire -1;
 * a single below-target frame restarts the count. */
ZTEST(agc_controller, test_attack_needs_consecutive_frames) {
    AgcController ctrl;
    FakeAgcConfig cfg;
    cfg.attack = 3;
    cfg.rate = 1; /* min-gap out of the way for this test */

    AgcDecision d;
    /* Two loud frames, then a quiet one: no step, run restarts. */
    d = ctrl.update(cfg, 0.02f, 1000, 0x28, true);
    zassert_equal(d.gain_steps, 0);
    d = ctrl.update(cfg, 0.02f, 1000, 0x28, true);
    zassert_equal(d.gain_steps, 0);
    d = ctrl.update(cfg, 0.001f, 1000, 0x28, true);
    zassert_equal(d.gain_steps, 0);
    /* Three consecutive loud frames: step on the third. */
    d = ctrl.update(cfg, 0.02f, 1000, 0x28, true);
    zassert_equal(d.gain_steps, 0);
    d = ctrl.update(cfg, 0.02f, 1000, 0x28, true);
    zassert_equal(d.gain_steps, 0);
    d = ctrl.update(cfg, 0.02f, 1000, 0x28, true);
    zassert_equal(d.gain_steps, -1, "third consecutive over-target frame must step");
}

/* Release: releaseFrames consecutive under-targetLow (smoothed) frames fire +1. */
ZTEST(agc_controller, test_release_after_sustained_quiet) {
    AgcController ctrl;
    FakeAgcConfig cfg;
    cfg.tlow = 0.01f;
    cfg.release = 4;
    cfg.rate = 1;

    int steps_at = -1;
    for (int i = 0; i < 6; i++) {
        /* 0.004 keeps smoothed well under tlow forever (max 0.004). */
        AgcDecision d = ctrl.update(cfg, 0.004f, 100, 0x28, true);
        if (d.gain_steps != 0) {
            zassert_equal(d.gain_steps, 1);
            steps_at = i;
            break;
        }
    }
    zassert_equal(steps_at, 3, "release must fire on the releaseFrames-th quiet frame (got %d)",
                  steps_at);
}

/* Min-gap: with rateLimitFrames = 5 and permanently loud input, steps land at
 * most once per 5 frames. */
ZTEST(agc_controller, test_rate_limit_min_gap) {
    AgcController ctrl;
    FakeAgcConfig cfg;
    cfg.attack = 1;
    cfg.rate = 5;

    int steps = 0;
    for (int i = 0; i < 20; i++) {
        AgcDecision d = ctrl.update(cfg, 0.02f, 1000, 0x28, true);
        if (d.gain_steps != 0) {
            steps++;
        }
    }
    zassert_equal(steps, 4, "20 loud frames at min-gap 5 must yield 4 steps (got %d)", steps);
}

/* Near-clip fast path: immediate -2, bypassing the rate limit; clamped to the
 * gain floor. */
ZTEST(agc_controller, test_clip_fast_path) {
    AgcController ctrl;
    FakeAgcConfig cfg;
    cfg.rate = 100; /* enormous min-gap: only the clip path may act */

    AgcDecision d = ctrl.update(cfg, 0.3f, 32100, 0x28, true);
    zassert_true(d.clipped);
    zassert_equal(d.gain_steps, -2, "clip must step -2 immediately");
    /* Next clipped frame steps again — no rate limit on the clip path. */
    d = ctrl.update(cfg, 0.3f, 32100, 0x26, true);
    zassert_equal(d.gain_steps, -2);
    /* At one step above the floor only -1 remains; at the floor, nothing. */
    d = ctrl.update(cfg, 0.3f, 32100, 0x01, true);
    zassert_equal(d.gain_steps, -1);
    d = ctrl.update(cfg, 0.3f, 32100, 0x00, true);
    zassert_true(d.clipped);
    zassert_equal(d.gain_steps, 0);
}

/* Noise gate: silence sets the flag, blocks the release path, and after the
 * park delay drifts gain toward 0 dB one step per interval, stopping there. */
ZTEST(agc_controller, test_silence_gates_release_and_parks) {
    AgcController ctrl;
    FakeAgcConfig cfg;
    cfg.gate = 0.001f;
    cfg.tlow = 0.01f; /* silence is also under targetLow — release must NOT fire */
    cfg.release = 4;
    cfg.rate = 1;

    uint8_t gain = 0x30; /* +4 dB — parked value is 0x28 */
    int frames_to_first_park = -1;
    int park_steps = 0;
    for (int i = 0; i < 1000; i++) {
        AgcDecision d = ctrl.update(cfg, 0.0f, 0, gain, true);
        zassert_true(d.silent, "silence must be flagged (frame %d)", i);
        if (d.gain_steps != 0) {
            zassert_equal(d.gain_steps, -1, "park drift must move toward 0x28");
            gain = (uint8_t)(gain + d.gain_steps);
            park_steps++;
            if (frames_to_first_park < 0) {
                frames_to_first_park = i;
            }
        }
    }
    zassert_equal(gain, AgcController::kGainPark, "must end parked at 0 dB (got 0x%02x)", gain);
    zassert_equal(park_steps, 8, "0x30 -> 0x28 is 8 drift steps (got %d)", park_steps);
    zassert_true(frames_to_first_park >= AgcController::kParkAfterSilentFrames - 1,
                 "park must wait ~10 s of silence (first step at frame %d)",
                 frames_to_first_park);
}

/* The noise gate is INPUT-REFERRED: the same absolute RMS is silence at max
 * gain (where it's just amplified mic noise) but signal at park gain.
 * Regression for the hardware-found failure where a quiet room's amplified
 * noise floor sat above an output-domain gate and the release path climbed to
 * +20 dB with beats firing on noise. */
ZTEST(agc_controller, test_gate_is_input_referred) {
    AgcController ctrl;
    FakeAgcConfig cfg;
    cfg.gate = 0.001f;
    cfg.tlow = 0.01f; /* everything below targetLow: only the gate blocks release */
    cfg.release = 2;
    cfg.rate = 1;

    /* rms 0.003 at MAX gain: input-referred 0.003 x 10^-1 = 0.0003 < gate →
     * silent; the release path must NOT step further up. */
    AgcDecision d = {};
    for (int i = 0; i < 10; i++) {
        d = ctrl.update(cfg, 0.003f, 100, AgcController::kGainMax, true);
        zassert_true(d.silent, "amplified noise floor at max gain must read as silence");
        zassert_true(d.gain_steps <= 0, "release must never step up while silent");
    }

    /* The same absolute rms at PARK gain is a real signal: gate open. */
    ctrl.reset();
    for (int i = 0; i < AgcController::kHistoryLen; i++) {
        d = ctrl.update(cfg, 0.003f, 100, AgcController::kGainPark, true);
    }
    zassert_false(d.silent, "the same level at 0 dB is signal, not silence");
}

/* Frozen (allow_adjust = false): levels and silence stay live, never a step. */
ZTEST(agc_controller, test_frozen_never_steps) {
    AgcController ctrl;
    FakeAgcConfig cfg;
    cfg.attack = 1;
    cfg.gate = 0.001f;

    for (int i = 0; i < 50; i++) {
        AgcDecision d = ctrl.update(cfg, 0.3f, 32100, 0x28, false);
        zassert_equal(d.gain_steps, 0, "frozen controller must never step");
        zassert_false(d.clipped);
    }
    /* Silence detection still works while frozen. */
    AgcDecision d = {};
    for (int i = 0; i < 40; i++) {
        d = ctrl.update(cfg, 0.0f, 0, 0x28, false);
    }
    zassert_true(d.silent, "silence must be reported even when frozen");
}

/* notifyGainChange rescales the RMS window into the new gain domain. */
ZTEST(agc_controller, test_notify_gain_change_rescales_window) {
    AgcController ctrl;
    FakeAgcConfig cfg;
    cfg.thigh = 1.0f; /* keep attack quiet */

    for (int i = 0; i < AgcController::kHistoryLen; i++) {
        ctrl.update(cfg, 0.01f, 100, 0x28, true);
    }
    float before = ctrl.smoothedRms();
    zassert_within(before, 0.01f, 0.0005f);

    ctrl.notifyGainChange(2); /* +1 dB amplitude = x1.0592537^2 */
    zassert_within(ctrl.smoothedRms(), before * 1.1220185f, 0.0005f,
                   "window must scale by the amplitude ratio (got %f)",
                   (double)ctrl.smoothedRms());

    /* A big jump (> 4 steps, e.g. manual "sound agc gain" across the range)
     * flushes the window instead of extrapolating — +40 steps would otherwise
     * fabricate impossible levels (RMS is bounded by 1.0). */
    ctrl.notifyGainChange(40);
    zassert_within(ctrl.smoothedRms(), 0.0f, 1e-9f,
                   "big jump must flush the window, not extrapolate (got %f)",
                   (double)ctrl.smoothedRms());
}

/* reset() zeroes the window and every policy counter (used after a PDM stream
 * restart, where the dead time invalidates the levels). */
ZTEST(agc_controller, test_reset_clears_state) {
    AgcController ctrl;
    FakeAgcConfig cfg;
    cfg.attack = 3;

    /* Two loud frames of attack run-up, a warm RMS window... */
    ctrl.update(cfg, 0.02f, 1000, 0x28, true);
    ctrl.update(cfg, 0.02f, 1000, 0x28, true);
    zassert_true(ctrl.smoothedRms() > 0.0f);

    ctrl.reset();
    zassert_within(ctrl.smoothedRms(), 0.0f, 1e-9f, "reset must zero the window");

    /* ...and the attack run must restart from zero: the next loud frame is
     * frame 1 of 3, so no step yet. */
    AgcDecision d = ctrl.update(cfg, 0.02f, 1000, 0x28, true);
    zassert_equal(d.gain_steps, 0, "attack run must not survive reset");
    /* notifyGainChange(0) is an explicit no-op. */
    float before = ctrl.smoothedRms();
    ctrl.notifyGainChange(0);
    zassert_within(ctrl.smoothedRms(), before, 1e-9f);
}

/* ── Noise-floor estimate (the Audio Tuning screen's most useful number) ─── */

/* Seeded from the first frame, not from zero. A min-tracker starting at 0 would take its
 * full ~5 minute rise time to reach the real floor after every boot, reporting a silent
 * room the whole way up. */
ZTEST(agc_controller, test_noise_floor_seeds_from_the_first_frame) {
    AgcController ctrl;
    FakeAgcConfig cfg;

    zassert_equal(ctrl.noiseFloor(), 0.0f, "unseeded floor must read zero, not a guess");

    /* Still filling the smoothing window: smoothed_ is a mean over a window that starts
     * full of zeros, so there is nothing honest to report yet. */
    for (int i = 0; i < AgcController::kHistoryLen - 1; i++) {
        ctrl.update(cfg, 0.004f, 100, AgcController::kGainPark, true);
        zassert_equal(ctrl.noiseFloor(), 0.0f, "must not seed from an unconverged window");
    }

    /* Window full — now the level means something and the floor seeds from it. */
    ctrl.update(cfg, 0.004f, 100, AgcController::kGainPark, true);
    zassert_within(ctrl.noiseFloor(), 0.004f, 0.004f * 0.02f,
                   "seeds from the settled level, not from a partially-filled window");
}

/* Down-fast is what makes it a FLOOR rather than an average: one quiet moment is evidence
 * about the room, one loud moment is not. */
ZTEST(agc_controller, test_noise_floor_drops_immediately_but_rises_slowly) {
    AgcController ctrl;
    FakeAgcConfig cfg;

    /* Settle on a moderate level so the window is full and the floor is seeded there. */
    for (int i = 0; i < 64; i++) {
        ctrl.update(cfg, 0.004f, 200, AgcController::kGainPark, true);
    }
    const float settled = ctrl.noiseFloor();
    zassert_within(settled, 0.004f, 0.004f * 0.05f, "floor seeds at the settled level");

    /* Go loud. The floor must NOT chase it — that is the whole point of a min-tracker.
     *
     * State the bound against the GAP, not against the floor: the tracker closes a fixed
     * FRACTION of the remaining distance each frame (1e-4, a ~320 s constant), so over 64
     * frames it covers ~0.64% of however far away the loud level is. Expressed as a
     * percentage of the floor that same motion looks much larger when the gap is wide,
     * which is what made an earlier "< 1.05x the floor" version of this assertion fail on
     * correct code. */
    const float kLoud = 0.05f;
    for (int i = 0; i < 64; i++) {
        ctrl.update(cfg, kLoud, 900, AgcController::kGainPark, true);
    }
    zassert_true(ctrl.noiseFloor() < settled + (kLoud - settled) * 0.02f,
                 "2 s of loud must close well under 2%% of the gap: floor %f, settled %f, "
                 "loud %f",
                 (double)ctrl.noiseFloor(), (double)settled, (double)kLoud);

    /* Now go genuinely quiet. The floor must follow down promptly, because one quiet
     * moment IS evidence about the room in a way one loud moment is not. */
    for (int i = 0; i < 64; i++) {
        ctrl.update(cfg, 0.0001f, 5, AgcController::kGainPark, true);
    }
    zassert_true(ctrl.noiseFloor() < settled / 10.0f,
                 "floor must follow the room down quickly: got %f from %f",
                 (double)ctrl.noiseFloor(), (double)settled);
}

/* Same reasoning as the noise gate itself: the room does not get louder because the AGC
 * turned up, so the floor must be gain-invariant. */
ZTEST(agc_controller, test_noise_floor_is_input_referred) {
    FakeAgcConfig cfg;
    AgcController parked;
    AgcController amplified;

    /* Same acoustic level, two different gains. At +10 dB the captured RMS is larger by
     * the amplitude ratio, but the INPUT-referred floor should land in the same place. */
    const int kSteps = 20; /* +10 dB */
    const float kAcoustic = 0.001f;
    const float amplified_rms = kAcoustic * audio_dsp_gain_amplitude_ratio(kSteps);

    for (int i = 0; i < 64; i++) {
        parked.update(cfg, kAcoustic, 50, AgcController::kGainPark, false);
        amplified.update(cfg, amplified_rms, 50, (uint8_t)(AgcController::kGainPark + kSteps),
                         false);
    }

    zassert_within(amplified.noiseFloor(), parked.noiseFloor(), parked.noiseFloor() * 0.02f,
                   "input-referred floor must not depend on where the AGC sits: %f vs %f",
                   (double)amplified.noiseFloor(), (double)parked.noiseFloor());
}

/* It is a measurement, not a control action — freezing the AGC must not blind it. */
ZTEST(agc_controller, test_noise_floor_tracks_while_frozen) {
    AgcController ctrl;
    FakeAgcConfig cfg;

    for (int i = 0; i < 32; i++) {
        ctrl.update(cfg, 0.002f, 100, AgcController::kGainPark, false /* frozen */);
    }
    zassert_true(ctrl.noiseFloor() > 0.0f, "frozen must still measure the room");
}

/* ── framesSinceStep (how settled the AGC is) ────────────────────────────── */

ZTEST(agc_controller, test_frames_since_step_counts_and_resets_on_a_step) {
    AgcController ctrl;
    FakeAgcConfig cfg;
    cfg.rate = 1;
    cfg.attack = 1;

    for (int i = 0; i < 5; i++) {
        ctrl.update(cfg, 0.001f, 100, AgcController::kGainPark, true);
    }
    zassert_equal(ctrl.framesSinceStep(), 5);

    /* A clip forces a step, which restarts the count. */
    const AgcDecision d =
        ctrl.update(cfg, 0.001f, AgcController::kClipPeak, AgcController::kGainPark, true);
    zassert_true(d.gain_steps < 0, "clip must step down");
    zassert_equal(ctrl.framesSinceStep(), 0, "a gain step restarts the settle clock");
}

ZTEST(agc_controller, test_reset_clears_the_new_telemetry_state) {
    AgcController ctrl;
    FakeAgcConfig cfg;

    for (int i = 0; i < 32; i++) {
        ctrl.update(cfg, 0.002f, 100, AgcController::kGainPark, true);
    }
    zassert_true(ctrl.noiseFloor() > 0.0f);
    zassert_true(ctrl.framesSinceStep() > 0);

    /* reset() runs after a PDM stream restart; stale telemetry across that would misreport
     * the room for the next five minutes. */
    ctrl.reset();
    zassert_equal(ctrl.noiseFloor(), 0.0f);
    zassert_equal(ctrl.framesSinceStep(), 0);

    /* And it must re-seed from a refilled window rather than crawl up from zero. */
    for (int i = 0; i < AgcController::kHistoryLen; i++) {
        ctrl.update(cfg, 0.004f, 100, AgcController::kGainPark, true);
    }
    zassert_within(ctrl.noiseFloor(), 0.004f, 0.004f * 0.02f);
}

ZTEST_SUITE(agc_controller, NULL, NULL, NULL, NULL, NULL);
