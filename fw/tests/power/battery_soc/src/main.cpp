/*
 * Tests for battery_soc_percent() (fw/src/battery_soc.h, power plan PR E):
 * endpoint clamps, monotonicity across the whole input range, and agreement
 * with the companion app's curve (app/services/battery.ts voltageToPercent)
 * at the shared anchor points.
 */

#include <battery_soc.h>
#include <zephyr/ztest.h>

/* The function is constexpr — pin the endpoints at compile time too. */
static_assert(battery_soc_percent(7000) == 0);
static_assert(battery_soc_percent(8400) == 100);

ZTEST_SUITE(battery_soc_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(battery_soc_tests, test_endpoints_and_clamps) {
    /* Exact endpoints. */
    zassert_equal(battery_soc_percent(7000), 0);
    zassert_equal(battery_soc_percent(8400), 100);

    /* Clamped below/above, including nonsense inputs. */
    zassert_equal(battery_soc_percent(6999), 0);
    zassert_equal(battery_soc_percent(6000), 0);
    zassert_equal(battery_soc_percent(0), 0);
    zassert_equal(battery_soc_percent(-1000), 0);
    zassert_equal(battery_soc_percent(8401), 100);
    zassert_equal(battery_soc_percent(20000), 100);
}

ZTEST(battery_soc_tests, test_monotonic_nondecreasing) {
    /* Bounded sweep of the whole meaningful range at 1 mV resolution: the
     * estimate must never go down as the voltage goes up. */
    uint8_t prev = battery_soc_percent(6500);
    for (int32_t mv = 6501; mv <= 8500; mv++) {
        uint8_t cur = battery_soc_percent(mv);
        zassert_true(cur >= prev, "non-monotonic at %d mV: %u < %u", mv, cur, prev);
        zassert_true(cur <= 100, "out of range at %d mV: %u", mv, cur);
        prev = cur;
    }
}

ZTEST(battery_soc_tests, test_matches_app_curve_anchor_points) {
    /* Anchor points shared with app/services/battery.ts DISCHARGE_CURVE_2S —
     * firmware and app must report the same percentage for these voltages. */
    zassert_equal(battery_soc_percent(7220), 5);
    zassert_equal(battery_soc_percent(7370), 10);
    zassert_equal(battery_soc_percent(7490), 25);
    zassert_equal(battery_soc_percent(7670), 50);
    zassert_equal(battery_soc_percent(7970), 75);
    zassert_equal(battery_soc_percent(8160), 85);

    /* Interpolated point the app's tests also use: 7910 mV = 70%. */
    zassert_equal(battery_soc_percent(7910), 70);
}
