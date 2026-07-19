/*
 * Tests for boot_gate (issue #208): the BT-free semaphore gate bt_thread_func()
 * waits on before its first BLE advertising start, signaled by
 * pattern_controller_thread_func() once boot-time work (extension discovery) is
 * done. Exercises the real k_sem-backed implementation directly - no BT stack
 * needed.
 *
 * boot_gate's single semaphore is process-global (file-scope static in
 * boot_gate.cpp) with no reset API - by design, since production code only
 * ever calls notify_ready() once. All three scenarios below therefore run as
 * one ordered sequence within a single test case, rather than independent
 * ZTEST cases, so each scenario's expected starting state (sem count) follows
 * deterministically from the one before it instead of relying on ztest
 * execution order.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <bluetooth/boot_gate.h>

namespace {

K_THREAD_STACK_DEFINE(notifier_stack, 512);
struct k_thread notifier_thread;

constexpr int32_t kNotifyDelayMs = 200;

void notifier_thread_fn(void *, void *, void *) {
    k_msleep(kNotifyDelayMs);
    boot_gate_notify_ready();
}

}  // namespace

ZTEST(boot_gate, test_scenarios_in_sequence) {
    // 1) Notify before anyone waits: a later wait must return true immediately
    // (semaphore semantics - no missed-wakeup race).
    boot_gate_notify_ready();
    zassert_true(boot_gate_wait_ready(0), "wait should succeed immediately after an earlier notify");

    // 2) Nobody notifies: wait must time out and return false, well after the
    // requested timeout has elapsed but without hanging forever.
    int64_t beforeMs = k_uptime_get();
    bool timedOutResult = boot_gate_wait_ready(50);
    int64_t elapsedMs = k_uptime_get() - beforeMs;
    zassert_false(timedOutResult, "wait should time out when nobody calls notify_ready()");
    zassert_true(elapsedMs >= 50, "wait returned before its timeout elapsed (%lld ms)", elapsedMs);

    // 3) A real blocking wait: notify arrives asynchronously from another thread
    // after a delay: wait must block for (at least) that long, then return true.
    k_thread_create(&notifier_thread, notifier_stack, K_THREAD_STACK_SIZEOF(notifier_stack),
                    notifier_thread_fn, NULL, NULL, NULL, K_PRIO_COOP(5), 0, K_NO_WAIT);

    beforeMs = k_uptime_get();
    bool notifiedResult = boot_gate_wait_ready(kNotifyDelayMs * 10);
    elapsedMs = k_uptime_get() - beforeMs;

    zassert_true(notifiedResult, "wait should succeed once the other thread calls notify_ready()");
    zassert_true(elapsedMs >= kNotifyDelayMs,
                "wait returned before the notifying thread's delay elapsed (%lld ms)", elapsedMs);

    k_thread_join(&notifier_thread, K_FOREVER);
}

ZTEST_SUITE(boot_gate, NULL, NULL, NULL, NULL, NULL);
