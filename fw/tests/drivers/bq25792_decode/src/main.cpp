/*
 * Unit tests for the BQ25792 ADC register unit-conversion classes (issue #97).
 *
 * The current-measurement registers (IBUS_ADC/IBAT_ADC) are 16-bit two's
 * complement at 1 mA/LSB; a missing sign-extension in
 * BQ25792_ADC_CURRENT_UnitConversion made every discharge (negative) current
 * read as a huge positive value. These tests pin the decode behavior for both
 * the current and voltage conversion classes without needing an I2C emulator —
 * the register accessors around them are thin enough that on-device
 * verification covers the plumbing.
 */

#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>

LOG_MODULE_REGISTER(bq25792_decode_tests);

// bq25792_dev_config must be complete before bq25792_priv.h's register wrappers
#include "bq25792/bq25792_init.h"

// Must be included after LOG_MODULE_REGISTER() since this contains LOG_XXX statements
// (same ordering requirement as bq25792.cpp)
#include "bq25792/bq25792_priv.h"

ZTEST_SUITE(bq25792_decode_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(bq25792_decode_tests, test_current_positive_passthrough) {
    zassert_equal(BQ25792_ADC_CURRENT_UnitConversion::conversion(0), 0);
    zassert_equal(BQ25792_ADC_CURRENT_UnitConversion::conversion(200), 200);
    zassert_equal(BQ25792_ADC_CURRENT_UnitConversion::conversion(0x7FFF), 32767);
}

ZTEST(bq25792_decode_tests, test_current_sign_extension) {
    /* 0xFFFF = -1 mA, 0xFF38 = -200 mA, 0x8000 = -32768 mA */
    zassert_equal(BQ25792_ADC_CURRENT_UnitConversion::conversion(0xFFFF), -1);
    zassert_equal(BQ25792_ADC_CURRENT_UnitConversion::conversion(0xFF38), -200);
    zassert_equal(BQ25792_ADC_CURRENT_UnitConversion::conversion(0x8000), -32768);
}

ZTEST(bq25792_decode_tests, test_current_unit_label) {
    zassert_equal(strcmp(BQ25792_ADC_CURRENT_UnitConversion::unit(), "mA"), 0);
}

ZTEST(bq25792_decode_tests, test_voltage_positive_passthrough) {
    /* 1 mV/LSB: a full 2S pack reads 8400 */
    zassert_equal(BQ25792_ADC_VOLTAGE_UnitConversion::conversion(0), 0);
    zassert_equal(BQ25792_ADC_VOLTAGE_UnitConversion::conversion(8400), 8400);
}

ZTEST(bq25792_decode_tests, test_voltage_sign_extension) {
    zassert_equal(BQ25792_ADC_VOLTAGE_UnitConversion::conversion(0xFFFF), -1);
}
