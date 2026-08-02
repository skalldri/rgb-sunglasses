#pragma once

#include <cstdint>

/**
 * @file
 * @brief Pure budget logic for the extension sandbox handshake (issue #276).
 *
 * Dependency-free by design (no Zephyr headers) so the native_sim suite
 * `extensions.tick_budget` can compile and exercise it on the host — same
 * seam as led_stats_core.h / conn_param_governor_core.h / factory_reset_core.h.
 *
 * The host budgets the CPU time the sandbox thread itself consumes, not the
 * elapsed wall time of the handshake, because the sandbox is the
 * lowest-priority application thread and elapsed time therefore bills it for
 * whatever preempted it. The full rationale, the measured numbers, and the
 * invariants that constrain any future retune live in ONE place:
 * fw/docs/threading.md, "The extension sandbox's scheduling latency is
 * unbounded — by design". Do not restate the argument here.
 */

namespace extension_host {

/** @brief Outcome of evaluating one in-progress or finished handshake. */
enum class TickVerdict : uint8_t {
    /** Not finished yet, but still inside both budgets — keep waiting. */
    Running,
    /** The sandbox signalled done within budget. */
    Completed,
    /** The sandbox thread itself burned more CPU than one tick may use.
     *  Spinning code accrues CPU fast, so this trips at roughly the budget
     *  regardless of system load. */
    CpuBudgetExceeded,
    /** The sandbox thread terminated without signalling done — it took a CPU
     *  fault and Zephyr's fatal handler aborted it (the issue #85 containment
     *  path). Detected from thread state rather than inferred from a timeout,
     *  so a crash is reported as a crash instead of masquerading as a hang. */
    SandboxDied,
    /** Never signalled done and the wall-clock deadline expired. The
     *  extension is blocked on something rather than spinning — it burns no
     *  CPU, so the CPU budget alone would never fire. */
    WallBackstopExceeded,
};

/** @brief Budget for one handshake, in hardware cycles. */
struct TickBudgetLimits {
    /** Max CPU the sandbox thread may consume in one tick. */
    uint64_t cpuBudgetCyc = 0;
};

/** @brief One observation of an in-progress or finished handshake. */
struct TickBudgetSample {
    /** True once the sandbox has signalled the done-semaphore. */
    bool completed = false;
    /** True once the sandbox thread has terminated. Only meaningful when
     *  `completed` is false — a thread that finished its tick and then died
     *  still produced a valid frame. */
    bool sandboxDied = false;
    /** True once the caller's wall-clock deadline has passed. The deadline is
     *  owned by the host (one shared timepoint per tick(), so a lazy load plus
     *  its follow-up tick can never together exceed one backstop) rather than
     *  recomputed here. */
    bool wallDeadlineExpired = false;
    /** CPU cycles consumed by the sandbox THREAD since the request was
     *  posted — not elapsed time. Near zero for a starved or blocked sandbox. */
    uint64_t cpuCyc = 0;
};

/**
 * @brief Classify one handshake observation against its budget.
 *
 * Evaluation order matters and is deliberate:
 *  1. CPU overrun wins even when the tick completed — an extension that
 *     finishes but burned more than a full tick's CPU budget has still
 *     broken the contract the render loop depends on.
 *  2. Completion — a tick that delivered a frame is honoured even if the
 *     sandbox died immediately afterwards; the frame is still valid.
 *  3. Sandbox death, which is a fact about thread state rather than a
 *     threshold, so it is reported as soon as it is observed instead of
 *     waiting out the deadline.
 *  4. The wall deadline, reachable only by a tick that never completed, never
 *     died, and never accrued enough CPU to trip rule 1.
 *
 * Starvation — the #276 bug — lands in none of the fault branches: a starved
 * tick has low cpuCyc and eventually completes, so it returns Running until
 * it does, then Completed.
 */
constexpr TickVerdict evaluate_tick_budget(const TickBudgetSample &sample,
                                           const TickBudgetLimits &limits) {
    if (sample.cpuCyc > limits.cpuBudgetCyc) {
        return TickVerdict::CpuBudgetExceeded;
    }
    if (sample.completed) {
        return TickVerdict::Completed;
    }
    if (sample.sandboxDied) {
        return TickVerdict::SandboxDied;
    }
    if (sample.wallDeadlineExpired) {
        return TickVerdict::WallBackstopExceeded;
    }
    return TickVerdict::Running;
}

/**
 * @brief True for every verdict that must tear the sandbox down.
 *
 * The host tests this rather than `!= Completed` so that adding a new
 * non-fault terminal verdict cannot silently start faulting slots.
 */
constexpr bool verdict_is_fault(TickVerdict verdict) {
    return verdict == TickVerdict::CpuBudgetExceeded || verdict == TickVerdict::SandboxDied ||
           verdict == TickVerdict::WallBackstopExceeded;
}

/*
 * NOTE — there is deliberately no per-verdict "should this reset params?"
 * helper. An earlier revision had one that spared WallBackstopExceeded, on the
 * reasoning that low CPU means the extension barely ran so a parameter cannot
 * be to blame. That is inverted: an extension that BLOCKS burns no CPU
 * precisely BECAUSE a parameter sent it down a waiting path (a UINT32 feeding a
 * sleep, a STRING naming data that never arrives, an index selecting a branch
 * that waits forever). Sparing that case left the slot permanently dead — the
 * retry reloaded the same poisoned blob, and it survived reboot. Every
 * TICK-TIME fault resets params, exactly as it did before issue #276; only
 * load/init-time failures do not, because params are copied into the
 * extension's inputs at tick time and so cannot have caused them. See
 * sandbox_fault() in extension_host.cpp.
 */

/**
 * @brief Min/avg/max accumulator for per-tick cycle costs.
 *
 * `ext stats` keeps one for wall time and one for CPU time; holding them apart
 * is the point (issue #276).
 */
struct TickStatAccumulator {
    uint32_t minCyc = 0;
    uint32_t maxCyc = 0;
    uint64_t sumCyc = 0;
    uint32_t count = 0;

    constexpr void reset() { *this = TickStatAccumulator{}; }

    constexpr void record(uint32_t cyc) {
        /* First sample seeds min — a UINT32_MAX sentinel would leak into the
         * report if the accumulator were ever printed before any tick. */
        if (count == 0 || cyc < minCyc) {
            minCyc = cyc;
        }
        if (cyc > maxCyc) {
            maxCyc = cyc;
        }
        sumCyc += cyc;
        count++;
    }

    /** @return Mean cycles, or 0 when nothing has been recorded (never divides
     *  by zero — `ext stats` may run before the first tick). */
    constexpr uint32_t avgCyc() const {
        return count == 0 ? 0u : static_cast<uint32_t>(sumCyc / count);
    }
};

}  // namespace extension_host
