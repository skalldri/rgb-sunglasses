/*
 * Tests for the out-of-tree BMI270 emulator (fw/drivers/emul_bmi270).
 *
 * These exercise the REAL upstream bmi270 driver end-to-end on native_sim:
 * driver init runs against the emulator at boot (chip id, soft reset,
 * config-file upload, INTERNAL_STATUS poll), and the tests then drive the
 * public sensor API while injecting samples through the sensor emulator
 * backend (emul_sensor_backend_set_channel).
 *
 * Two Twister scenarios share this file (see testcase.yaml):
 *  - drivers.emul_bmi270:         poll mode (CONFIG_BMI270_TRIGGER_NONE)
 *  - drivers.emul_bmi270.trigger: CONFIG_BMI270_TRIGGER_OWN_THREAD; the
 *    data-ready test fires INT2 via native_sim's gpio_emul. Tests that
 *    don't apply to the current scenario skip themselves.
 */

#include <math.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/emul_sensor.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "emul_bmi270.h"

/* Register addresses/values, mirroring the driver's private bmi270.h */
#define REG_ACC_CONF     0x40
#define REG_ACC_RANGE    0x41
#define REG_GYR_CONF     0x42
#define REG_GYR_RANGE    0x43
#define REG_INT_MAP_DATA 0x58
#define REG_PWR_CTRL     0x7D

#define ACC_ODR_25_HZ  0x06
#define GYR_ODR_25_HZ  0x06
#define ODR_MSK        0x0F
#define PWR_CTRL_GYR_EN 0x02
#define PWR_CTRL_ACC_EN 0x04
#define INT_MAP_DATA_DRDY_INT2 BIT(6)

static const struct device *const bmi_dev = DEVICE_DT_GET(DT_NODELABEL(bmi270));
static const struct emul *const bmi_emul = EMUL_DT_GET(DT_NODELABEL(bmi270));

/* Convert a q31 + shift fixed-point value (the backend API's format) to a
 * plain double for comparisons. Doubles are fine on native_sim.
 */
static double q31_to_double(q31_t value, int8_t shift)
{
	return ldexp((double)value, shift - 31);
}

ZTEST(emul_bmi270, test_device_ready)
{
	/* device_is_ready passing means bmi270_init() completed against the
	 * emulator: SPI-mode dummy read, chip id 0x24, soft reset, PWR_CONF
	 * read-modify-write, config-file upload handshake, and the
	 * INTERNAL_STATUS INIT_OK poll.
	 */
	zassert_true(device_is_ready(bmi_dev));
}

ZTEST(emul_bmi270, test_config_file_uploaded)
{
	/* Our devicetree compatible ("bosch,bmi270", not "bosch,bmi270-base")
	 * selects the driver's 328-byte max_fifo config file, uploaded in
	 * 256-byte bursts: ceil(328 / 256) * 256 = 512 bytes on the wire.
	 * Catches regressions in the emul's SPI burst-write framing.
	 */
	zassert_equal(emul_bmi270_cfg_bytes_written(bmi_emul), 512);
}

ZTEST(emul_bmi270, test_odr_attr)
{
	/* The exact configuration imu_thread applies: 25 Hz, val2 zeroed
	 * (an uninitialized val2 once powered the sensor off — see the
	 * comment in fw/src/imu/imu.cpp).
	 */
	struct sensor_value odr = {.val1 = 25, .val2 = 0};
	uint8_t reg;

	zassert_ok(sensor_attr_set(bmi_dev, SENSOR_CHAN_ACCEL_XYZ,
				   SENSOR_ATTR_SAMPLING_FREQUENCY, &odr));
	zassert_ok(sensor_attr_set(bmi_dev, SENSOR_CHAN_GYRO_XYZ,
				   SENSOR_ATTR_SAMPLING_FREQUENCY, &odr));

	zassert_ok(emul_bmi270_get_reg(bmi_emul, REG_ACC_CONF, &reg, 1));
	zassert_equal(reg & ODR_MSK, ACC_ODR_25_HZ, "ACC_CONF ODR bits: 0x%02x", reg);

	zassert_ok(emul_bmi270_get_reg(bmi_emul, REG_GYR_CONF, &reg, 1));
	zassert_equal(reg & ODR_MSK, GYR_ODR_25_HZ, "GYR_CONF ODR bits: 0x%02x", reg);

	zassert_ok(emul_bmi270_get_reg(bmi_emul, REG_PWR_CTRL, &reg, 1));
	zassert_equal(reg & (PWR_CTRL_ACC_EN | PWR_CTRL_GYR_EN),
		      PWR_CTRL_ACC_EN | PWR_CTRL_GYR_EN,
		      "PWR_CTRL should enable accel+gyro: 0x%02x", reg);
}

/* Inject a value on each axis of a channel triple, fetch through the driver,
 * and check the round-tripped SI values match within the backend's epsilon.
 */
static void inject_and_check(enum sensor_channel chan_base, enum sensor_channel chan_xyz)
{
	q31_t lower, upper, epsilon;
	int8_t shift;
	struct sensor_chan_spec spec = {.chan_type = chan_base, .chan_idx = 0};

	zassert_ok(emul_sensor_backend_get_sample_range(bmi_emul, spec, &lower,
							&upper, &epsilon, &shift));

	/* Per-axis test values: 0 on X, half scale on Y, near negative full
	 * scale on Z — distinct values also prove axis ordering in the
	 * 12-byte burst read.
	 */
	q31_t values[3] = {0, upper / 2, (q31_t)(-((int64_t)upper * 9 / 10))};

	for (int axis = 0; axis < 3; axis++) {
		spec.chan_type = chan_base + axis;
		zassert_ok(emul_sensor_backend_set_channel(bmi_emul, spec,
							   &values[axis], shift));
	}

	zassert_ok(sensor_sample_fetch(bmi_dev));

	struct sensor_value out[3];

	zassert_ok(sensor_channel_get(bmi_dev, chan_xyz, out));

	double eps = q31_to_double(epsilon, shift);

	for (int axis = 0; axis < 3; axis++) {
		double expected = q31_to_double(values[axis], shift);
		double actual = sensor_value_to_double(&out[axis]);

		zassert_within(actual, expected, eps,
			       "chan %d axis %d: expected %f, got %f (eps %f)",
			       chan_base, axis, expected, actual, eps);
	}
}

ZTEST(emul_bmi270, test_accel_inject_and_scale)
{
	static const int ranges_g[] = {2, 4, 8, 16};

	for (size_t i = 0; i < ARRAY_SIZE(ranges_g); i++) {
		struct sensor_value fs = {.val1 = ranges_g[i], .val2 = 0};

		zassert_ok(sensor_attr_set(bmi_dev, SENSOR_CHAN_ACCEL_XYZ,
					   SENSOR_ATTR_FULL_SCALE, &fs),
			   "setting accel range %d g", ranges_g[i]);
		inject_and_check(SENSOR_CHAN_ACCEL_X, SENSOR_CHAN_ACCEL_XYZ);
	}
}

ZTEST(emul_bmi270, test_gyro_inject_and_scale)
{
	static const int ranges_dps[] = {125, 250, 500, 1000, 2000};

	for (size_t i = 0; i < ARRAY_SIZE(ranges_dps); i++) {
		struct sensor_value fs = {.val1 = ranges_dps[i], .val2 = 0};

		zassert_ok(sensor_attr_set(bmi_dev, SENSOR_CHAN_GYRO_XYZ,
					   SENSOR_ATTR_FULL_SCALE, &fs),
			   "setting gyro range %d dps", ranges_dps[i]);
		inject_and_check(SENSOR_CHAN_GYRO_X, SENSOR_CHAN_GYRO_XYZ);
	}
}

#if defined(CONFIG_BMI270_TRIGGER)

static K_SEM_DEFINE(drdy_sem, 0, 1);

static void drdy_handler(const struct device *dev, const struct sensor_trigger *trig)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(trig);
	k_sem_give(&drdy_sem);
}

#endif /* CONFIG_BMI270_TRIGGER */

ZTEST(emul_bmi270, test_drdy_trigger)
{
#if defined(CONFIG_BMI270_TRIGGER)
	/* The driver maps SENSOR_TRIG_DATA_READY to INT2 = irq-gpios index 1
	 * = gpio0 pin 1 in our overlay (INT1/index 0 is feature interrupts).
	 */
	const struct device *const gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	static const struct sensor_trigger trig = {
		.type = SENSOR_TRIG_DATA_READY,
		.chan = SENSOR_CHAN_ALL,
	};
	uint8_t reg;

	zassert_true(device_is_ready(gpio_dev));
	k_sem_reset(&drdy_sem);

	zassert_ok(sensor_trigger_set(bmi_dev, &trig, drdy_handler));

	/* The real bmi270_trigger.c path ran: it routed data-ready to INT2 */
	zassert_ok(emul_bmi270_get_reg(bmi_emul, REG_INT_MAP_DATA, &reg, 1));
	zassert_equal(reg & INT_MAP_DATA_DRDY_INT2, INT_MAP_DATA_DRDY_INT2,
		      "INT_MAP_DATA: 0x%02x", reg);

	/* Fire INT2 (edge to active) and expect the handler on the driver's
	 * own trigger thread; repeat to prove it re-arms.
	 */
	for (int i = 0; i < 2; i++) {
		zassert_ok(gpio_emul_input_set(gpio_dev, 1, 1));
		zassert_ok(k_sem_take(&drdy_sem, K_MSEC(100)),
			   "data-ready trigger %d did not fire", i);
		zassert_ok(gpio_emul_input_set(gpio_dev, 1, 0));
	}

	/* Handler removal must also succeed (imu_thread never does this, but
	 * it completes the trigger_set contract).
	 */
	zassert_ok(sensor_trigger_set(bmi_dev, &trig, NULL));
#else
	ztest_test_skip();
#endif /* CONFIG_BMI270_TRIGGER */
}

ZTEST_SUITE(emul_bmi270, NULL, NULL, NULL, NULL, NULL);
