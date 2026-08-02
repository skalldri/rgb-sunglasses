/* Unit tests for AgcController (fw/src/sound/agc_controller.cpp) — the real
 * firmware AGC policy, exercised deterministically frame by frame. */
#include <zephyr/ztest.h>

#include "agc_controller.h"

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

ZTEST_SUITE(agc_controller, NULL, NULL, NULL, NULL, NULL);
