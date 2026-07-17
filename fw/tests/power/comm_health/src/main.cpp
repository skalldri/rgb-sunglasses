/*
 * Unit tests for the charger comm-error hysteresis (fw/src/power/comm_health):
 * N consecutive failed reads enter the error state (event fires exactly once),
 * one success exits. power.cpp keys the status LED, the BLE 0xFF sentinel and
 * the poll backoff off these events.
 */

#include <zephyr/ztest.h>

#include "power/comm_health.h"

namespace {
constexpr uint32_t kEntryTicks = 5;
}

ZTEST(comm_health, test_stays_healthy_on_success) {
    CommHealth h;
    for (int i = 0; i < 10; i++) {
        zassert_equal(comm_health_tick(h, true, kEntryTicks), CommHealthEvent::None);
    }
    zassert_false(h.in_error);
    zassert_equal(h.fail_ticks, 0);
}

ZTEST(comm_health, test_enters_error_exactly_at_threshold_and_only_once) {
    CommHealth h;

    for (uint32_t i = 0; i < kEntryTicks - 1; i++) {
        zassert_equal(comm_health_tick(h, false, kEntryTicks), CommHealthEvent::None,
                      "entered early at tick %u", i + 1);
        zassert_false(h.in_error);
    }

    zassert_equal(comm_health_tick(h, false, kEntryTicks), CommHealthEvent::EnteredError);
    zassert_true(h.in_error);

    /* Continued failures stay in the state without re-firing the event. */
    for (int i = 0; i < 20; i++) {
        zassert_equal(comm_health_tick(h, false, kEntryTicks), CommHealthEvent::None);
        zassert_true(h.in_error);
    }
}

ZTEST(comm_health, test_recovers_on_first_success) {
    CommHealth h;
    for (uint32_t i = 0; i < kEntryTicks; i++) {
        comm_health_tick(h, false, kEntryTicks);
    }
    zassert_true(h.in_error);

    zassert_equal(comm_health_tick(h, true, kEntryTicks), CommHealthEvent::Recovered);
    zassert_false(h.in_error);
    zassert_equal(h.fail_ticks, 0);

    /* Recovered fires once; staying healthy is None. */
    zassert_equal(comm_health_tick(h, true, kEntryTicks), CommHealthEvent::None);
}

ZTEST(comm_health, test_intermittent_failures_below_threshold_never_enter) {
    CommHealth h;

    /* Repeated bursts of entry_ticks-1 failures, each broken by a success —
     * the counter must reset on every good read. */
    for (int burst = 0; burst < 4; burst++) {
        for (uint32_t i = 0; i < kEntryTicks - 1; i++) {
            zassert_equal(comm_health_tick(h, false, kEntryTicks), CommHealthEvent::None);
        }
        zassert_equal(comm_health_tick(h, true, kEntryTicks), CommHealthEvent::None,
                      "burst %d spuriously entered/recovered", burst);
        zassert_false(h.in_error);
    }
}

ZTEST(comm_health, test_flap_cycle_fires_paired_events) {
    CommHealth h;

    for (int cycle = 0; cycle < 3; cycle++) {
        for (uint32_t i = 0; i < kEntryTicks - 1; i++) {
            zassert_equal(comm_health_tick(h, false, kEntryTicks), CommHealthEvent::None);
        }
        zassert_equal(comm_health_tick(h, false, kEntryTicks), CommHealthEvent::EnteredError,
                      "cycle %d missing entry", cycle);
        zassert_equal(comm_health_tick(h, true, kEntryTicks), CommHealthEvent::Recovered,
                      "cycle %d missing recovery", cycle);
    }
}

ZTEST(comm_health, test_fail_counter_saturates) {
    CommHealth h;
    h.fail_ticks = UINT32_MAX;
    h.in_error = true;

    /* Must not wrap back to 0 (which would look like "no failures"). */
    zassert_equal(comm_health_tick(h, false, kEntryTicks), CommHealthEvent::None);
    zassert_equal(h.fail_ticks, UINT32_MAX);
    zassert_true(h.in_error);
}

ZTEST_SUITE(comm_health, NULL, NULL, NULL, NULL, NULL);
