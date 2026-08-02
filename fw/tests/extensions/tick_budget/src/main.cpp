/*
 * Unit tests for the extension sandbox tick-budget logic (issue #276).
 *
 * The behaviour under test is the distinction the old wall-clock deadline
 * could not make: an extension that is SLOW (burning CPU) versus one that is
 * merely STARVED (preempted by higher-priority threads). The former must
 * fault; the latter must not.
 */

#include <extensions/extension_tick_budget.h>
#include <zephyr/ztest.h>

using extension_host::evaluate_tick_budget;
using extension_host::TickBudgetLimits;
using extension_host::TickBudgetSample;
using extension_host::TickStatAccumulator;
using extension_host::TickVerdict;
using extension_host::verdict_is_fault;

namespace {

/* Stand-in for the shipped 50 ms CPU budget, in arbitrary "cycle" units at a
 * notional 1000 cycles/ms. */
constexpr TickBudgetLimits kLimits{50'000};

/* `wallExpired` stands in for the host's shared deadline having passed. The
 * host owns that timepoint (one sys_timepoint_calc per tick(), shared with the
 * lazy load's handshake), so this layer only ever sees the boolean. */
constexpr TickBudgetSample sample(bool completed, uint64_t cpuCyc, bool wallExpired,
                                 bool sandboxDied = false) {
    TickBudgetSample s;
    s.completed = completed;
    s.sandboxDied = sandboxDied;
    s.cpuCyc = cpuCyc;
    s.wallDeadlineExpired = wallExpired;
    return s;
}

}  // namespace

ZTEST_SUITE(extension_tick_budget, NULL, NULL, NULL, NULL, NULL);

/* --- evaluate_tick_budget ------------------------------------------------ */

ZTEST(extension_tick_budget, test_in_progress_tick_keeps_waiting) {
    const auto v = evaluate_tick_budget(sample(false, 1'000, false), kLimits);
    zassert_equal(v, TickVerdict::Running, "a cheap unfinished tick must keep waiting");
}

ZTEST(extension_tick_budget, test_completed_tick_within_budget) {
    const auto v = evaluate_tick_budget(sample(true, 4'700, false), kLimits);
    zassert_equal(v, TickVerdict::Completed);
    zassert_false(verdict_is_fault(v));
}

ZTEST(extension_tick_budget, test_runaway_tick_trips_cpu_budget) {
    /* Spinning extension: CPU accrues past the budget, never completes. */
    const auto v = evaluate_tick_budget(sample(false, 50'001, false), kLimits);
    zassert_equal(v, TickVerdict::CpuBudgetExceeded);
    zassert_true(verdict_is_fault(v));
}

ZTEST(extension_tick_budget, test_cpu_overrun_beats_completion) {
    /* Finishing does not excuse blowing the budget: a tick that consumed more
     * CPU than a whole frame is a runaway whether or not it returned. */
    const auto v = evaluate_tick_budget(sample(true, 60'000, false), kLimits);
    zassert_equal(v, TickVerdict::CpuBudgetExceeded,
                  "completion must not mask a CPU-budget overrun");
}

ZTEST(extension_tick_budget, test_blocked_tick_trips_wall_deadline) {
    /* Blocked, not spinning: no CPU consumed at all, so only the wall deadline
     * can catch it. */
    const auto v = evaluate_tick_budget(sample(false, 0, true), kLimits);
    zassert_equal(v, TickVerdict::WallBackstopExceeded);
    zassert_true(verdict_is_fault(v));
}

/*
 * THE issue #276 REGRESSION TEST.
 *
 * Hardware numbers from the faulting board: Plasma's own per-tick cost was
 * ~4700 us while one led_display frame took ~41473 us, pushing the handshake
 * to 43457 us against a 50 ms wall deadline. Under the old rule that was a
 * near-miss that faulted as soon as any frame ran slightly longer. Under CPU
 * budgeting the same tick is unremarkable: the extension is nowhere near its
 * CPU budget, and the shared wall deadline (500 ms) has not expired.
 */
ZTEST(extension_tick_budget, test_starved_tick_is_not_a_fault) {
    /* Mid-starvation: tens of ms of wall time gone, but the extension has only
     * had 4.7 ms of CPU and the deadline has not passed. */
    const auto midway = evaluate_tick_budget(sample(false, 4'700, false), kLimits);
    zassert_equal(midway, TickVerdict::Running,
                  "a starved tick must not fault while it is still under its CPU budget");

    /* And when it finally gets scheduled, it completes normally. */
    const auto done = evaluate_tick_budget(sample(true, 4'700, false), kLimits);
    zassert_equal(done, TickVerdict::Completed);
    zassert_false(verdict_is_fault(done));
}

/*
 * A CPU fault inside the extension aborts the sandbox thread (issue #85
 * containment), so the done-semaphore is never given. Detecting the dead
 * thread reports a crash as a crash and does it immediately, rather than
 * letting it masquerade as a hang until the deadline.
 */
ZTEST(extension_tick_budget, test_crashed_sandbox_is_detected_immediately) {
    const auto v =
        evaluate_tick_budget(sample(false, 1'200, false, /*sandboxDied=*/true), kLimits);
    zassert_equal(v, TickVerdict::SandboxDied);
    zassert_true(verdict_is_fault(v));
}

ZTEST(extension_tick_budget, test_completed_tick_survives_a_late_death) {
    /* Signalled done, then the thread went away. The frame was already
     * produced, so the tick counts — the death is next tick's problem. */
    const auto v = evaluate_tick_budget(sample(true, 4'700, false, /*sandboxDied=*/true), kLimits);
    zassert_equal(v, TickVerdict::Completed);
    zassert_false(verdict_is_fault(v));
}

ZTEST(extension_tick_budget, test_death_outranks_an_expired_deadline) {
    /* Both true: report the crash, which is a fact, rather than the timeout,
     * which is only an inference — and the two carry different log text. */
    const auto v = evaluate_tick_budget(sample(false, 1'200, true, /*sandboxDied=*/true), kLimits);
    zassert_equal(v, TickVerdict::SandboxDied);
}

ZTEST(extension_tick_budget, test_cpu_budget_boundary_is_exclusive) {
    /* Exactly at budget is still legal; one cycle over is not. */
    zassert_equal(evaluate_tick_budget(sample(true, 50'000, false), kLimits),
                  TickVerdict::Completed);
    zassert_equal(evaluate_tick_budget(sample(true, 50'001, false), kLimits),
                  TickVerdict::CpuBudgetExceeded);
}

ZTEST(extension_tick_budget, test_expired_deadline_does_not_mask_completion) {
    /* The deadline expiring in the same poll that the tick completed must not
     * throw away a delivered frame. */
    const auto v = evaluate_tick_budget(sample(true, 4'700, true), kLimits);
    zassert_equal(v, TickVerdict::Completed);
}

/* --- fault classification ------------------------------------------------ */

ZTEST(extension_tick_budget, test_only_faults_are_faults) {
    zassert_false(verdict_is_fault(TickVerdict::Running));
    zassert_false(verdict_is_fault(TickVerdict::Completed));
    zassert_true(verdict_is_fault(TickVerdict::CpuBudgetExceeded));
    zassert_true(verdict_is_fault(TickVerdict::SandboxDied));
    zassert_true(verdict_is_fault(TickVerdict::WallBackstopExceeded));
}

/*
 * evaluate_tick_budget() is constexpr, so a call with literal arguments is
 * constant-folded and never appears in the generated code - the assertions
 * above still verify the logic, but they verify it at COMPILE time, which
 * leaves the runtime branches unexercised (and invisible to lcov). Route the
 * inputs through volatile so the compiler must emit and run the real
 * comparison chain, covering every branch including SandboxDied.
 */
ZTEST(extension_tick_budget, test_all_branches_at_runtime) {
    struct Case {
        bool completed;
        bool died;
        bool wallExpired;
        uint64_t cpu;
        TickVerdict want;
    };
    static const Case cases[] = {
        {false, false, false, 1'000, TickVerdict::Running},
        {true, false, false, 4'700, TickVerdict::Completed},
        {false, false, false, 50'001, TickVerdict::CpuBudgetExceeded},
        {true, false, false, 60'000, TickVerdict::CpuBudgetExceeded},
        {false, true, false, 1'200, TickVerdict::SandboxDied},
        {true, true, false, 4'700, TickVerdict::Completed},
        {false, false, true, 0, TickVerdict::WallBackstopExceeded},
        {false, true, true, 1'200, TickVerdict::SandboxDied},
    };

    for (const auto &c : cases) {
        volatile bool completed = c.completed;
        volatile bool died = c.died;
        volatile bool wallExpired = c.wallExpired;
        volatile uint64_t cpu = c.cpu;
        volatile uint64_t cpuLimit = kLimits.cpuBudgetCyc;

        TickBudgetSample s;
        s.completed = completed;
        s.sandboxDied = died;
        s.wallDeadlineExpired = wallExpired;
        s.cpuCyc = cpu;
        TickBudgetLimits l;
        l.cpuBudgetCyc = cpuLimit;

        zassert_equal(evaluate_tick_budget(s, l), c.want,
                      "runtime evaluation disagreed for cpu=%llu", (unsigned long long)c.cpu);
    }
}

/* --- TickStatAccumulator -------------------------------------------------- */

ZTEST(extension_tick_budget, test_empty_accumulator_reports_zero) {
    /* `ext stats` can run before the first tick - must not divide by zero or
     * leak a min sentinel. */
    TickStatAccumulator acc;
    zassert_equal(acc.count, 0);
    zassert_equal(acc.avgCyc(), 0);
    zassert_equal(acc.minCyc, 0);
    zassert_equal(acc.maxCyc, 0);
}

ZTEST(extension_tick_budget, test_first_sample_seeds_min_and_max) {
    TickStatAccumulator acc;
    acc.record(4'700);
    zassert_equal(acc.count, 1);
    zassert_equal(acc.minCyc, 4'700, "first sample must seed min, not compare against 0");
    zassert_equal(acc.maxCyc, 4'700);
    zassert_equal(acc.avgCyc(), 4'700);
}

ZTEST(extension_tick_budget, test_tracks_min_avg_max) {
    TickStatAccumulator acc;
    acc.record(4'272);
    acc.record(43'457);
    acc.record(4'700);
    zassert_equal(acc.count, 3);
    zassert_equal(acc.minCyc, 4'272);
    zassert_equal(acc.maxCyc, 43'457);
    zassert_equal(acc.avgCyc(), (4'272u + 43'457u + 4'700u) / 3u);
}

ZTEST(extension_tick_budget, test_reset_clears_everything) {
    TickStatAccumulator acc;
    acc.record(1'234);
    acc.record(5'678);
    acc.reset();
    zassert_equal(acc.count, 0);
    zassert_equal(acc.avgCyc(), 0);
    zassert_equal(acc.minCyc, 0);
    zassert_equal(acc.maxCyc, 0);

    /* After a reset the next sample must seed min again, exactly as on a
     * freshly constructed accumulator (this is what activation does). */
    acc.record(9'000);
    zassert_equal(acc.minCyc, 9'000);
    zassert_equal(acc.maxCyc, 9'000);
}

ZTEST(extension_tick_budget, test_long_run_average_does_not_overflow) {
    /* The running sum is 64-bit on purpose: a 30 fps animation left running
     * accumulates far past what a uint32 sum could hold. */
    TickStatAccumulator acc;
    for (uint32_t i = 0; i < 100'000; i++) {
        acc.record(100'000);
    }
    zassert_equal(acc.count, 100'000);
    zassert_equal(acc.avgCyc(), 100'000, "sum must not wrap");
    zassert_equal(acc.sumCyc, (uint64_t)100'000 * 100'000);
}
