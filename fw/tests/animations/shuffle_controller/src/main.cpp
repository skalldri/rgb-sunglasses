#include <animations/shuffle_controller.h>
#include <zephyr/ztest.h>

#include <cstdint>

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

namespace {

// Convenient built-in ids for pool entries (values only — no animation code linked).
constexpr Animation kA = Animation::ZigZag;
constexpr Animation kB = Animation::Text;
constexpr Animation kC = Animation::Rainbow;

struct FakePool : public ShuffleAnimationPool {
    Animation ids[8] = {};
    size_t n = 0;
    Animation ineligible[4] = {};
    size_t nIneligible = 0;

    void set(const Animation *list, size_t count) {
        n = count;
        for (size_t i = 0; i < count; i++) {
            ids[i] = list[i];
        }
    }

    size_t count() override { return n; }
    Animation idAt(size_t index) override { return index < n ? ids[index] : Animation::None; }
    bool isEligible(Animation id) override {
        if (id == Animation::None) {
            return false;
        }
        for (size_t i = 0; i < nIneligible; i++) {
            if (ineligible[i] == id) {
                return false;
            }
        }
        return true;
    }
};

struct FakeConfig : public ShuffleConfigSource {
    bool en = true;
    uint32_t minS = 1;
    uint32_t maxS = 1;

    bool enabled() override { return en; }
    uint32_t minDurationS() override { return minS; }
    uint32_t maxDurationS() override { return maxS; }
};

// Scripted RNG: ShuffleRandomFn is a plain function pointer, so the script is static.
// Values are consumed in order; once exhausted the rng returns 0 forever.
uint32_t sRngSeq[32];
size_t sRngLen;
size_t sRngIdx;

uint32_t scripted_rng() {
    return (sRngIdx < sRngLen) ? sRngSeq[sRngIdx++] : 0u;
}

void rng_reset() {
    sRngLen = 0;
    sRngIdx = 0;
}

void rng_script(const uint32_t *vals, size_t count) {
    rng_reset();
    for (size_t i = 0; i < count && i < 32; i++) {
        sRngSeq[i] = vals[i];
    }
    sRngLen = count;
}

// Standard 4-entry pool: {None, A, B, C}.
void default_pool(FakePool &pool) {
    const Animation ids[] = {Animation::None, kA, kB, kC};
    pool.set(ids, 4);
}

// Runs frames of `dtMs` with a constant good flag until the controller decides to
// switch or `maxFrames` elapse. Returns the number of frames consumed (arm frame NOT
// included — call onFrame once to arm before using this) and fills `out`.
size_t run_until_switch(ShuffleController &ctrl, Animation current, uint32_t dtMs, bool good,
                        size_t maxFrames, ShuffleController::Decision &out,
                        uint32_t requestedGraceMs = 0) {
    for (size_t i = 1; i <= maxFrames; i++) {
        out = ctrl.onFrame(current, dtMs, good, requestedGraceMs);
        if (out.switchNow) {
            return i;
        }
    }
    return maxFrames + 1;
}

void suite_before(void *) {
    rng_reset();
}

}  // namespace

ZTEST_SUITE(shuffle_controller, NULL, NULL, suite_before, NULL, NULL);

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

ZTEST(shuffle_controller, test_disabled_never_switches) {
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.en = false;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 0);

    for (int i = 0; i < 1000; i++) {
        const auto d = ctrl.onFrame(kA, 1000, true);
        zassert_false(d.switchNow, "disabled shuffle must never switch");
    }
}

ZTEST(shuffle_controller, test_min_eq_max_exact_timing) {
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 5;
    cfg.maxS = 5;
    ShuffleController ctrl(pool, cfg, scripted_rng, 30000);

    // Arm frame: no switch, dwell starts at 0.
    auto d = ctrl.onFrame(kA, 1000, true);
    zassert_false(d.switchNow, "arm frame must not switch");

    // 4 x 1000 ms < 5000 ms: not yet.
    for (int i = 0; i < 4; i++) {
        d = ctrl.onFrame(kA, 1000, true);
        zassert_false(d.switchNow, "switched %d ms before the exact 5 s target", 4000 - i * 1000);
    }

    // 5th frame reaches exactly 5000 ms: switch now (good is true).
    d = ctrl.onFrame(kA, 1000, true);
    zassert_true(d.switchNow, "must switch the frame dwell reaches min==max");
    zassert_true(d.next != Animation::None && d.next != kA, "picked an invalid next");
}

ZTEST(shuffle_controller, test_random_target_in_range) {
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 2;
    cfg.maxS = 5;  // span 4: targets 2..5 s

    // rng: arm target pick = 3 -> target (2 + 3 % 4) = 5 s.
    const uint32_t script[] = {3u, 0u, 0u};
    rng_script(script, 3);
    ShuffleController ctrl(pool, cfg, scripted_rng, 30000);

    ShuffleController::Decision d = ctrl.onFrame(kA, 1000, true);  // arm
    zassert_false(d.switchNow, "arm frame must not switch");
    const size_t frames = run_until_switch(ctrl, kA, 1000, true, 10, d);
    zassert_equal(frames, 5, "rng=3 with min=2 max=5 must give a 5 s target (got %zu s)", frames);

    // rng = 4 -> 4 % 4 = 0 -> target = min = 2 s.
    const uint32_t script2[] = {4u, 0u, 0u};
    rng_script(script2, 3);
    ShuffleController ctrl2(pool, cfg, scripted_rng, 30000);
    d = ctrl2.onFrame(kA, 1000, true);  // arm
    const size_t frames2 = run_until_switch(ctrl2, kA, 1000, true, 10, d);
    zassert_equal(frames2, 2, "rng=4 with min=2 max=5 must give a 2 s target (got %zu s)", frames2);
}

ZTEST(shuffle_controller, test_min_gt_max_swapped) {
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 10;  // inverted on purpose: effective range must become [2, 10]
    cfg.maxS = 2;

    // rng 0 -> target = swapped min = 2 s.
    ShuffleController ctrl(pool, cfg, scripted_rng, 30000);
    ShuffleController::Decision d = ctrl.onFrame(kA, 1000, true);  // arm
    zassert_false(d.switchNow, "arm frame must not switch");
    const size_t frames = run_until_switch(ctrl, kA, 1000, true, 15, d);
    zassert_equal(frames, 2, "min>max must swap to [2,10]; rng=0 -> 2 s target (got %zu)", frames);

    // rng 8 -> 8 % 9 = 8 -> target = 2 + 8 = 10 s (the swapped upper bound).
    const uint32_t script[] = {8u, 0u, 0u};
    rng_script(script, 3);
    ShuffleController ctrl2(pool, cfg, scripted_rng, 30000);
    d = ctrl2.onFrame(kA, 1000, true);  // arm
    const size_t frames2 = run_until_switch(ctrl2, kA, 1000, true, 15, d);
    zassert_equal(frames2, 10, "rng=8 must give the swapped 10 s upper bound (got %zu)", frames2);
}

ZTEST(shuffle_controller, test_good_moment_gating) {
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    // Huge grace so only the good flag can trigger the switch in this test.
    ShuffleController ctrl(pool, cfg, scripted_rng, 1000000000ull);

    auto d = ctrl.onFrame(kA, 1000, false);  // arm
    zassert_false(d.switchNow, "arm frame must not switch");

    // Past the 1 s target but never a good moment: must keep waiting.
    for (int i = 0; i < 20; i++) {
        d = ctrl.onFrame(kA, 1000, false);
        zassert_false(d.switchNow, "switched while good-moment was false (frame %d)", i);
    }

    // The first good-moment frame after the target: switch immediately.
    d = ctrl.onFrame(kA, 1000, true);
    zassert_true(d.switchNow, "must switch on the first good-moment frame past the target");
}

ZTEST(shuffle_controller, test_grace_cap_forces_switch) {
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 2000);  // grace = 2 s

    auto d = ctrl.onFrame(kA, 500, false);  // arm
    zassert_false(d.switchNow, "arm frame must not switch");

    // dwell: 500, 1000, ..., 2500 — all < target(1000) + grace(2000) = 3000 with good
    // false (dwell 1000..2500 are past the target but inside the grace window).
    for (int i = 0; i < 5; i++) {
        d = ctrl.onFrame(kA, 500, false);
        zassert_false(d.switchNow, "switched inside the grace window (dwell %d ms)",
                      500 * (i + 2));
    }

    // dwell reaches 3000 = target + grace: forced switch despite good == false.
    d = ctrl.onFrame(kA, 500, false);
    zassert_true(d.switchNow, "grace cap must force the switch at target+grace");
}

ZTEST(shuffle_controller, test_pick_excludes_none_and_current) {
    FakePool pool;
    default_pool(pool);  // {None, A, B, C}; eligible-and-not-current from A: {B, C}
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;

    // Script: arm rng, pick rng 0 -> first candidate (B), rearm rng.
    const uint32_t script[] = {0u, 0u, 0u};
    rng_script(script, 3);
    ShuffleController ctrl(pool, cfg, scripted_rng, 0);
    ShuffleController::Decision d = ctrl.onFrame(kA, 1000, true);  // arm
    d = ctrl.onFrame(kA, 1000, true);
    zassert_true(d.switchNow, "expected a switch at the 1 s target");
    zassert_equal((int)d.next, (int)kB, "pick rng=0 from {B,C} must be B");

    // Same again with pick rng 1 -> second candidate (C).
    const uint32_t script2[] = {0u, 1u, 0u};
    rng_script(script2, 3);
    ShuffleController ctrl2(pool, cfg, scripted_rng, 0);
    d = ctrl2.onFrame(kA, 1000, true);  // arm
    d = ctrl2.onFrame(kA, 1000, true);
    zassert_true(d.switchNow, "expected a switch at the 1 s target");
    zassert_equal((int)d.next, (int)kC, "pick rng=1 from {B,C} must be C");
}

ZTEST(shuffle_controller, test_pick_skips_ineligible) {
    FakePool pool;
    default_pool(pool);
    pool.ineligible[0] = kB;  // e.g. a faulted extension slot
    pool.nIneligible = 1;
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 0);

    // From A, the only candidate left is C — any pick rng value must yield it.
    auto d = ctrl.onFrame(kA, 1000, true);  // arm
    d = ctrl.onFrame(kA, 1000, true);
    zassert_true(d.switchNow, "expected a switch at the 1 s target");
    zassert_equal((int)d.next, (int)kC, "ineligible id must never be picked");
}

ZTEST(shuffle_controller, test_all_ineligible_no_switch) {
    // Issue #243: every candidate opted out of shuffle ("include in shuffle" false on
    // all of them) — like pool-of-one, shuffle must idle rather than switch or crash.
    FakePool pool;
    default_pool(pool);
    pool.ineligible[0] = kB;
    pool.ineligible[1] = kC;
    pool.nIneligible = 2;
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 0);

    ctrl.onFrame(kA, 1000, true);  // arm
    for (int i = 0; i < 100; i++) {
        const auto d = ctrl.onFrame(kA, 1000, true);
        zassert_false(d.switchNow, "all-ineligible pool must never switch (frame %d)", i);
    }
}

ZTEST(shuffle_controller, test_eligibility_flips_midrun) {
    // Issue #243: the include flag is PULLED at pick time, so a BLE toggle between
    // hops must take effect on the very next pick with no re-arm needed.
    FakePool pool;
    default_pool(pool);
    pool.ineligible[0] = kB;  // user excluded B
    pool.nIneligible = 1;
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 0);

    auto d = ctrl.onFrame(kA, 1000, true);  // arm
    d = ctrl.onFrame(kA, 1000, true);
    zassert_true(d.switchNow, "expected a switch at the 1 s target");
    zassert_equal((int)d.next, (int)kC, "excluded id must never be picked");

    // User re-includes B mid-run. The controller already tracked its own switch
    // (lastSeen_ = C) and re-armed a fresh 1 s target, so the next frame on C
    // reaches that target and picks again: candidates from C are now {A, B} and the
    // scripted pick rng 1 must land on B (proving the flag was re-read at pick time).
    pool.nIneligible = 0;
    const uint32_t script[] = {1u, 0u, 0u};
    rng_script(script, 3);
    d = ctrl.onFrame(kC, 1000, true);
    zassert_true(d.switchNow, "expected a switch at the re-armed 1 s target");
    zassert_equal((int)d.next, (int)kB, "re-included id must be pickable again");
}

ZTEST(shuffle_controller, test_current_becomes_ineligible) {
    // Issue #243: excluding the CURRENTLY-PLAYING animation must not pin shuffle on
    // it — the dwell still expires and shuffle hops away to an eligible candidate.
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 2;
    cfg.maxS = 2;
    ShuffleController ctrl(pool, cfg, scripted_rng, 0);

    auto d = ctrl.onFrame(kA, 1000, true);  // arm
    pool.ineligible[0] = kA;                // user excludes A while it is playing
    pool.nIneligible = 1;
    d = ctrl.onFrame(kA, 1000, true);  // dwell 1000
    zassert_false(d.switchNow, "must not switch before the target");
    d = ctrl.onFrame(kA, 1000, true);  // dwell 2000 = target
    zassert_true(d.switchNow, "must still switch away from a newly-excluded current");
    zassert_true(d.next == kB || d.next == kC, "next must be an eligible non-current id");
}

ZTEST(shuffle_controller, test_pool_of_one_no_switch) {
    FakePool pool;
    const Animation ids[] = {Animation::None, kA};
    pool.set(ids, 2);  // only the current animation is eligible
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 0);

    ctrl.onFrame(kA, 1000, true);  // arm
    for (int i = 0; i < 100; i++) {
        const auto d = ctrl.onFrame(kA, 1000, true);
        zassert_false(d.switchNow, "pool-of-1 must never switch (frame %d)", i);
    }
}

ZTEST(shuffle_controller, test_pool_of_zero_no_switch) {
    FakePool pool;  // empty
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 0);

    ctrl.onFrame(kA, 1000, true);  // arm
    for (int i = 0; i < 100; i++) {
        const auto d = ctrl.onFrame(kA, 1000, true);
        zassert_false(d.switchNow, "empty pool must never switch (frame %d)", i);
    }
}

ZTEST(shuffle_controller, test_manual_change_resets_dwell) {
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 4;
    cfg.maxS = 4;
    ShuffleController ctrl(pool, cfg, scripted_rng, 30000);

    auto d = ctrl.onFrame(kA, 1000, true);  // arm on A
    d = ctrl.onFrame(kA, 1000, true);       // dwell 1000
    d = ctrl.onFrame(kA, 1000, true);       // dwell 2000
    zassert_false(d.switchNow, "must not have switched before the 4 s target");

    // Manual change to B mid-dwell: this frame re-arms, and the 4 s clock restarts.
    d = ctrl.onFrame(kB, 1000, true);
    zassert_false(d.switchNow, "the frame after a manual change must only re-arm");

    for (int i = 0; i < 3; i++) {
        d = ctrl.onFrame(kB, 1000, true);  // dwell 1000..3000 on B
        zassert_false(d.switchNow, "dwell must restart from the manual change (frame %d)", i);
    }
    d = ctrl.onFrame(kB, 1000, true);  // dwell 4000 on B
    zassert_true(d.switchNow, "must switch 4 s after the manual change");
    zassert_true(d.next != kB && d.next != Animation::None, "picked an invalid next");
}

ZTEST(shuffle_controller, test_disable_midwait_resets) {
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 1000000000ull);

    ctrl.onFrame(kA, 1000, false);  // arm
    for (int i = 0; i < 3; i++) {
        ctrl.onFrame(kA, 1000, false);  // dwell past target, waiting on good-moment
    }

    cfg.en = false;
    auto d = ctrl.onFrame(kA, 1000, true);  // disabled: full reset, no switch
    zassert_false(d.switchNow, "a disabled frame must never switch");

    cfg.en = true;
    d = ctrl.onFrame(kA, 1000, true);  // re-enable: this frame only re-arms
    zassert_false(d.switchNow, "re-enable must start a fresh dwell, not resume the old one");

    d = ctrl.onFrame(kA, 1000, true);  // fresh dwell reaches the 1 s target
    zassert_true(d.switchNow, "must switch one full period after re-enable");
}

ZTEST(shuffle_controller, test_uint32_max_no_overflow) {
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = UINT32_MAX;
    cfg.maxS = UINT32_MAX;
    ShuffleController ctrl(pool, cfg, scripted_rng, 0);

    ctrl.onFrame(kA, UINT32_MAX, true);  // arm; target = UINT32_MAX * 1000 ms

    // 999 frames of UINT32_MAX ms each: 999 * (2^32 - 1) < (2^32 - 1) * 1000 — a wrap
    // anywhere in the ms math would make one of these switch early.
    ShuffleController::Decision d;
    for (int i = 0; i < 999; i++) {
        d = ctrl.onFrame(kA, UINT32_MAX, true);
        zassert_false(d.switchNow, "switched early — 64-bit dwell math wrapped (frame %d)", i);
    }

    // Frame 1000: dwell = 1000 * (2^32 - 1) ms = the target exactly.
    d = ctrl.onFrame(kA, UINT32_MAX, true);
    zassert_true(d.switchNow, "must switch once dwell reaches the uint32-max-seconds target");
}

// ---------------------------------------------------------------------------
// Animation-requested grace (long GLIM clips / long text scrolls)
//
// The animation states how far away its next natural boundary is, measured from NOW.
// The controller turns that into a forced-switch deadline latched once per dwell:
//   floor    = target + graceMs      (the unchanged anti-hang budget)
//   rolling  = dwell  + requested
//   cap      = target + maxGraceMs
//   deadline = min(max(floor, rolling), max(floor, cap))
// ---------------------------------------------------------------------------

ZTEST(shuffle_controller, test_requested_grace_ignored_without_max) {
    // The 4-arg ctor leaves maxGraceMs at 0, which disables animation-requested grace
    // entirely — the pre-issue-#250 behaviour, byte for byte, no matter how much an
    // animation asks for.
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 2000);  // grace 2 s, no ceiling

    auto d = ctrl.onFrame(kA, 500, false, 1000000u);  // arm
    zassert_false(d.switchNow, "arm frame must not switch");

    // dwell 500..2500: below target(1000) + grace(2000) = 3000.
    for (int i = 0; i < 5; i++) {
        d = ctrl.onFrame(kA, 500, false, 1000000u);
        zassert_false(d.switchNow, "switched inside the grace window (dwell %d ms)",
                      500 * (i + 2));
    }

    d = ctrl.onFrame(kA, 500, false, 1000000u);
    zassert_true(d.switchNow, "a request must be ignored when maxGraceMs is 0");
}

ZTEST(shuffle_controller, test_requested_grace_extends_past_global_cap) {
    // The core fix: an animation that says "my boundary is 10 s away" gets 10 s from
    // NOW, not the fixed 2 s past its target.
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 2000, 600000);  // grace 2 s, max 600 s

    auto d = ctrl.onFrame(kA, 1000, false, 10000u);  // arm
    zassert_false(d.switchNow, "arm frame must not switch");

    // Deadline latches on the first frame past the target (dwell 1000): 1000 + 10000.
    for (int i = 1; i <= 10; i++) {
        d = ctrl.onFrame(kA, 1000, false, 10000u);
        zassert_false(d.switchNow, "switched before the requested boundary (dwell %d ms)",
                      1000 * i);
    }

    d = ctrl.onFrame(kA, 1000, false, 10000u);
    zassert_true(d.switchNow, "must force the switch once the requested grace expires");
}

ZTEST(shuffle_controller, test_requested_grace_below_global_cap_uses_floor) {
    // A request smaller than the fixed cap must never SHORTEN the wait — the fixed cap
    // is a floor, not a default that a request replaces.
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 10000, 600000);  // grace 10 s

    auto d = ctrl.onFrame(kA, 1000, false, 1000u);  // arm; request only 1 s
    zassert_false(d.switchNow, "arm frame must not switch");

    // dwell 1000..10000: still inside the 1000 + 10000 floor, despite the 1 s request.
    for (int i = 1; i <= 10; i++) {
        d = ctrl.onFrame(kA, 1000, false, 1000u);
        zassert_false(d.switchNow, "a small request shortened the fixed cap (dwell %d ms)",
                      1000 * i);
    }

    d = ctrl.onFrame(kA, 1000, false, 1000u);
    zassert_true(d.switchNow, "must still force the switch at target + grace");
}

ZTEST(shuffle_controller, test_requested_grace_clamped_to_max) {
    // A clip longer than any sane ceiling is still cut — just far later.
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 2000, 5000);  // grace 2 s, max 5 s

    auto d = ctrl.onFrame(kA, 1000, false, 1000000u);  // arm; asks for 1000 s
    zassert_false(d.switchNow, "arm frame must not switch");

    // Deadline = target(1000) + maxGrace(5000) = 6000.
    for (int i = 1; i <= 5; i++) {
        d = ctrl.onFrame(kA, 1000, false, 1000000u);
        zassert_false(d.switchNow, "switched before the ceiling (dwell %d ms)", 1000 * i);
    }

    d = ctrl.onFrame(kA, 1000, false, 1000000u);
    zassert_true(d.switchNow, "an over-large request must be clamped to maxGraceMs");
}

ZTEST(shuffle_controller, test_shrinking_request_switches_at_clip_end) {
    // Regression for the reported bug: a ~3.5 min GLIM clip (bad_apple.glim) modelled at
    // 12 s. The animation restates a SHRINKING remaining time every frame, so a deadline
    // computed as `target + requested` would converge on the midpoint between the target
    // and the clip's end (D < target + (C - D) => D < (target + C) / 2 = 6500 here) and
    // still cut mid-clip. The latched, dwell-relative deadline must land on the clip end.
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 2000, 600000);

    uint32_t remainingMs = 12000;                              // clip time left at the arm frame
    auto d = ctrl.onFrame(kA, 1000, false, remainingMs);       // arm

    // Frames 1..11: dwell 1000..11000, remaining 11000..1000, never a good moment.
    for (int i = 1; i <= 11; i++) {
        remainingMs -= 1000;
        d = ctrl.onFrame(kA, 1000, false, remainingMs);
        zassert_false(d.switchNow,
                      "cut mid-clip at dwell %d ms (3000 = the old fixed cap, 7000 = the "
                      "target+requested midpoint trap)",
                      1000 * i);
    }

    // Frame 12: the clip's last frame elapses — the animation signals a good moment and
    // has nothing left to ask for.
    d = ctrl.onFrame(kA, 1000, true, 0u);
    zassert_true(d.switchNow, "must switch at the clip's natural end");
}

ZTEST(shuffle_controller, test_grace_deadline_latched_not_re_extended) {
    // Models GLIM PlayAll auto-advancing to a LONGER clip while shuffle is already
    // waiting: the new clip's remaining time must not chain onto this dwell.
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 2000, 600000);

    auto d = ctrl.onFrame(kA, 1000, false, 10000u);  // arm
    d = ctrl.onFrame(kA, 1000, false, 10000u);       // dwell 1000: latches deadline 11000
    zassert_false(d.switchNow, "must not switch on the latching frame");

    // Every later frame asks for the full ceiling — ignored, the deadline is latched.
    for (int i = 2; i <= 10; i++) {
        d = ctrl.onFrame(kA, 1000, false, 600000u);
        zassert_false(d.switchNow, "switched before the latched deadline (dwell %d ms)",
                      1000 * i);
    }

    d = ctrl.onFrame(kA, 1000, false, 600000u);
    zassert_true(d.switchNow, "a re-stated larger request must not extend a latched deadline");
}

ZTEST(shuffle_controller, test_grace_deadline_recomputed_after_switch) {
    // rearm() clears the latch, so the NEXT dwell gets its own (here: unrequested) window.
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = 1;
    cfg.maxS = 1;
    ShuffleController ctrl(pool, cfg, scripted_rng, 2000, 600000);

    ShuffleController::Decision d;
    ctrl.onFrame(kA, 1000, false, 10000u);  // arm
    const size_t frames = run_until_switch(ctrl, kA, 1000, false, 20, d, 10000u);
    zassert_equal(frames, 11, "first dwell must be forced at target + requested (got %zu)",
                  frames);

    // Second dwell on the animation shuffle just picked, this time requesting nothing.
    const Animation next = d.next;
    const size_t frames2 = run_until_switch(ctrl, next, 1000, false, 20, d, 0u);
    zassert_equal(frames2, 3, "second dwell must fall back to target + grace (got %zu)",
                  frames2);
}

ZTEST(shuffle_controller, test_requested_grace_no_overflow) {
    // Companion to test_uint32_max_no_overflow: the deadline math adds a uint32 request
    // to a uint64 dwell and a uint32-max-seconds target — all of it must stay 64-bit.
    FakePool pool;
    default_pool(pool);
    FakeConfig cfg;
    cfg.minS = UINT32_MAX;
    cfg.maxS = UINT32_MAX;
    ShuffleController ctrl(pool, cfg, scripted_rng, 0, 600000);

    ShuffleController::Decision d;
    ctrl.onFrame(kA, UINT32_MAX, false, UINT32_MAX);  // arm; target = UINT32_MAX * 1000 ms

    for (int i = 0; i < 999; i++) {
        d = ctrl.onFrame(kA, UINT32_MAX, false, UINT32_MAX);
        zassert_false(d.switchNow, "switched early — 64-bit deadline math wrapped (frame %d)",
                      i);
    }

    // Frame 1000: dwell == target exactly. The request (clamped to the 600 s ceiling)
    // pushes the deadline past it, so this frame must still not switch.
    d = ctrl.onFrame(kA, UINT32_MAX, false, UINT32_MAX);
    zassert_false(d.switchNow, "must wait out the clamped request past the target");

    d = ctrl.onFrame(kA, UINT32_MAX, false, UINT32_MAX);
    zassert_true(d.switchNow, "must force the switch once the clamped deadline passes");
}
