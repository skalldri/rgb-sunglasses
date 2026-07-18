/*
 * Unit tests for battery_util.h's battery_quantize() (issue #97 follow-up) —
 * the pure rounding helper the battery telemetry GATT characteristics use to
 * suppress BLE notify spam from ADC jitter (see the doc comment on
 * battery_quantize() in fw/src/battery_util.h for the full rationale).
 *
 * It was previously only indirectly exercised by a handful of cases bolted
 * onto tests/bluetooth/checked_write (an unrelated GATT-write suite) — this
 * is battery_util.h's own dedicated, discoverable suite, alongside its
 * fw/tests/power sibling battery_soc.
 *
 * Pure header-only integer logic - no board, no I2C emulator, no BT stack.
 * battery_quantize is constexpr, so its core behavior is checked at compile
 * time via static_assert below, in addition to the runtime ZTEST cases (which
 * double as a record of intent / regression coverage and are easier to
 * extend with new cases than a wall of static_asserts).
 */

#include <battery_util.h>
#include <zephyr/ztest.h>

/* ---- Compile-time checks (constexpr evaluation) ---- */

static_assert(battery_quantize(0, 10) == 0, "zero quantizes to zero");
static_assert(battery_quantize(7910, 10) == 7910, "exact multiple is unchanged");
static_assert(battery_quantize(7914, 10) == 7910, "rounds down below half");
static_assert(battery_quantize(7915, 10) == 7920, "half rounds up (away from zero)");
static_assert(battery_quantize(7916, 10) == 7920, "rounds up above half");
static_assert(battery_quantize(-14, 10) == -10, "negative rounds toward zero below half");
static_assert(battery_quantize(-15, 10) == -20, "negative half rounds away from zero, symmetric");
static_assert(battery_quantize(1234, 1) == 1234, "step of 1 is a no-op");
static_assert(battery_quantize(1234, 0) == 1234, "step of 0 is a no-op");
static_assert(battery_quantize(-5, -3) == -5, "negative step is treated as a no-op");

ZTEST_SUITE(battery_quantize_tests, NULL, NULL, NULL, NULL, NULL);

/* Degenerate steps (<= 1) must return the input unchanged, regardless of sign
 * or magnitude of value. */
ZTEST(battery_quantize_tests, test_step_le_1_is_noop) {
    zassert_equal(battery_quantize(1234, 1), 1234, "step == 1 must be a no-op");
    zassert_equal(battery_quantize(1234, 0), 1234, "step == 0 must be a no-op");
    zassert_equal(battery_quantize(1234, -5), 1234, "negative step must be a no-op");
    zassert_equal(battery_quantize(0, 0), 0, "zero value, zero step");
    zassert_equal(battery_quantize(-1234, 1), -1234, "negative value, step == 1 must be a no-op");
}

/* Zero input quantizes to zero for any valid step. */
ZTEST(battery_quantize_tests, test_zero_value) {
    zassert_equal(battery_quantize(0, 10), 0);
    zassert_equal(battery_quantize(0, 1000), 0);
}

/* Exact multiples of the step must be returned unchanged (no rounding drift). */
ZTEST(battery_quantize_tests, test_exact_multiples_unchanged) {
    zassert_equal(battery_quantize(7910, 10), 7910);
    zassert_equal(battery_quantize(0, 10), 0);
    zassert_equal(battery_quantize(-7910, 10), -7910);
    zassert_equal(battery_quantize(100, 100), 100);
}

/* Positive values round to nearest multiple of step; ties round up (away
 * from zero), matching the "round half up" semantics documented in the
 * header (mirrors the -14/-15 example, mirrored to positive numbers). */
ZTEST(battery_quantize_tests, test_positive_rounds_to_nearest) {
    zassert_equal(battery_quantize(7914, 10), 7910, "below half rounds down");
    zassert_equal(battery_quantize(7915, 10), 7920, "exactly half rounds up");
    zassert_equal(battery_quantize(7916, 10), 7920, "above half rounds up");
    zassert_equal(battery_quantize(4, 10), 0, "small value below half-step rounds to zero");
    zassert_equal(battery_quantize(5, 10), 10, "small value at half-step rounds up");
}

/* Negative values must round symmetrically to the positive case, preserving
 * sign (this is the charge/discharge direction bit the doc comment calls
 * out) rather than always rounding toward positive or negative infinity. */
ZTEST(battery_quantize_tests, test_negative_values_symmetric) {
    zassert_equal(battery_quantize(-14, 10), -10, "below half rounds toward zero");
    zassert_equal(battery_quantize(-15, 10), -20, "exactly half rounds away from zero");
    zassert_equal(battery_quantize(-16, 10), -20, "above half rounds away from zero");
    zassert_equal(battery_quantize(-4, 10), 0, "small negative below half-step rounds to zero");
    zassert_equal(battery_quantize(-5, 10), -10, "small negative at half-step rounds away from zero");
}

/* battery_quantize(-x, step) must equal -battery_quantize(x, step) for every
 * value in range - pins the "symmetric around zero" doc claim directly
 * rather than only via the hand-picked pairs above. */
ZTEST(battery_quantize_tests, test_negation_is_symmetric) {
    const int32_t step = 10;
    for (int32_t v = -50; v <= 50; v++) {
        zassert_equal(battery_quantize(-v, step), -battery_quantize(v, step),
                      "asymmetry at value=%d", v);
    }
}

/* Step need not be a power of ten; exercise odd steps (both a small one and
 * the one used by the alternate-step test below) to make sure the rounding
 * arithmetic isn't accidentally tied to base-10 values. */
ZTEST(battery_quantize_tests, test_alternate_step_sizes) {
    zassert_equal(battery_quantize(149, 100), 100, "step 100: below half rounds down");
    zassert_equal(battery_quantize(150, 100), 200, "step 100: exact half rounds up");
    zassert_equal(battery_quantize(151, 100), 200, "step 100: above half rounds up");
    zassert_equal(battery_quantize(-150, 100), -200, "step 100: negative half is symmetric");

    /* Odd step (half truncates down per step/2 integer division). */
    zassert_equal(battery_quantize(11, 7), 14, "step 7: half is 3 (7/2 truncated)");
    zassert_equal(battery_quantize(10, 7), 7, "step 7: value below the truncated half");
    zassert_equal(battery_quantize(7, 3), 6, "step 3: half is 1 (3/2 truncated)");
    zassert_equal(battery_quantize(8, 3), 9, "step 3: value at the truncated half");
    zassert_equal(battery_quantize(-7, 3), -6, "step 3: negative mirrors the positive case");
    zassert_equal(battery_quantize(-8, 3), -9, "step 3: negative mirrors the positive case");
}

/* Larger, realistic magnitudes (e.g. a ~4000 mA fast-charge current reading)
 * quantized at the 10 mA step used by battery_service.cpp, well away from
 * int32_t overflow, to cover values outside the small hand-picked examples
 * above. */
ZTEST(battery_quantize_tests, test_realistic_current_magnitudes) {
    zassert_equal(battery_quantize(4096, 10), 4100, "typical charge-current sample");
    zassert_equal(battery_quantize(-4096, 10), -4100, "typical discharge-current sample, symmetric");
    zassert_equal(battery_quantize(4090, 10), 4090, "exact multiple at realistic magnitude");
}

/* Realistic telemetry values from the battery service's actual call sites
 * (10 mV / 10 mA quantization step for vbat/ibat/vbus/ibus). */
ZTEST(battery_quantize_tests, test_realistic_battery_telemetry_values) {
    zassert_equal(battery_quantize(4152, 10), 4150, "vbat_mv");
    zassert_equal(battery_quantize(5003, 10), 5000, "vbus_mv");
    zassert_equal(battery_quantize(-487, 10), -490, "ibat_ma, discharging");
    zassert_equal(battery_quantize(213, 10), 210, "ibus_ma, charging");
}
