/*
 * End-to-end tests for the IMU pipeline (fw/src/imu/imu.cpp) on native_sim.
 *
 * imu.cpp's SYS_INIT hook runs at boot against the real upstream bmi270
 * driver, which talks to the out-of-tree emulator (fw/drivers/emul_bmi270).
 * The tests inject samples through the sensor emulator backend, fire the
 * data-ready interrupt (INT2 = gpio0 pin 1) via gpio_emul, and consume
 * imu_result_q exactly like the Tilt animation does.
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
#include "imu/imu.h"

/* Register addresses/values, mirroring the driver's private bmi270.h */
#define REG_ACC_CONF     0x40
#define REG_GYR_CONF     0x42
#define REG_INT_MAP_DATA 0x58
#define REG_PWR_CTRL     0x7D

#define ODR_25_HZ 0x06
#define ODR_MSK   0x0F
#define PWR_CTRL_GYR_EN 0x02
#define PWR_CTRL_ACC_EN 0x04
#define INT_MAP_DATA_DRDY_INT2 BIT(6)

static const struct emul *const bmi_emul = EMUL_DT_GET(DT_NODELABEL(bmi270));
static const struct device *const gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));

static double q31_to_double(q31_t value, int8_t shift)
{
	return ldexp((double)value, shift - 31);
}

/* Pulse INT2 (data-ready). The driver arms an edge-to-active interrupt, so
 * each fire is a full low->high->low pulse. The sleep must outlast the whole
 * trigger-thread + fetch + msgq-put chain: the driver's SPI reads carry
 * k_usleep delays that each round up to a full 10 ms tick on native_sim, so
 * back-to-back pulses spaced only one tick apart coalesce on the driver's
 * semaphore and frames get dropped.
 */
static void fire_drdy(void)
{
	zassert_ok(gpio_emul_input_set(gpio_dev, 1, 1));
	k_msleep(50);
	zassert_ok(gpio_emul_input_set(gpio_dev, 1, 0));
}

/* Per-channel-triple injection state: values as q31 with the backend's shift */
struct injected {
	q31_t values[3];
	q31_t epsilon;
	int8_t shift;
};

/* Inject distinct per-axis values scaled from the backend-reported range:
 * 0 on X, half scale on Y, near negative full scale on Z.
 */
static void inject_triple(enum sensor_channel chan_base, struct injected *inj)
{
	q31_t lower, upper;
	struct sensor_chan_spec spec = {.chan_type = chan_base, .chan_idx = 0};

	zassert_ok(emul_sensor_backend_get_sample_range(bmi_emul, spec, &lower,
							&upper, &inj->epsilon,
							&inj->shift));

	inj->values[0] = 0;
	inj->values[1] = upper / 2;
	inj->values[2] = (q31_t)(-((int64_t)upper * 9 / 10));

	for (int axis = 0; axis < 3; axis++) {
		spec.chan_type = chan_base + axis;
		zassert_ok(emul_sensor_backend_set_channel(bmi_emul, spec,
							   &inj->values[axis],
							   inj->shift));
	}
}

static void check_triple(const struct injected *inj, const float *actual,
			 const char *what)
{
	double eps = q31_to_double(inj->epsilon, inj->shift);

	for (int axis = 0; axis < 3; axis++) {
		double expected = q31_to_double(inj->values[axis], inj->shift);

		/* No %f: CONFIG_CBPRINTF_FP_SUPPORT is off, print micro-units */
		zassert_within((double)actual[axis], expected, eps,
			       "%s axis %d: expected %d, got %d (eps %d, µunits)",
			       what, axis, (int)(expected * 1e6),
			       (int)((double)actual[axis] * 1e6),
			       (int)(eps * 1e6));
	}
}

static void drain_queue(void *fixture)
{
	ARG_UNUSED(fixture);

	struct imu_analysis_result discard;

	while (k_msgq_get(&imu_result_q, &discard, K_NO_WAIT) == 0) {
	}
}

/* imu_thread configures the sensor once at startup (ODR + power enables).
 * Each register write in that sequence carries the driver's 1 ms inter-write
 * delay, which k_usleep rounds up to a full tick on native_sim, so the whole
 * startup takes ~200 ms of simulated time — and it only begins once the ztest
 * (cooperative) thread first yields. Wait for its final register write
 * (PWR_CTRL gaining both enables) before running any test.
 */
static void *wait_for_imu_thread_startup(void)
{
	uint8_t want = PWR_CTRL_ACC_EN | PWR_CTRL_GYR_EN;
	uint8_t reg = 0;

	for (int i = 0; i < 100; i++) {
		zassert_ok(emul_bmi270_get_reg(bmi_emul, REG_PWR_CTRL, &reg, 1));
		if ((reg & want) == want) {
			return NULL;
		}
		k_msleep(10);
	}

	zassert_unreachable("imu_thread never configured PWR_CTRL: 0x%02x", reg);
	return NULL;
}

ZTEST(imu_pipeline, test_boot_configured)
{
	/* imu_init() (SYS_INIT) and imu_thread's startup already ran at boot:
	 * verify their register-visible effects in the emulator. The ODR
	 * checks lock in the uninitialized-val2 regression (25 Hz with
	 * garbage val2 lands outside every ODR bin and powers the sensor
	 * off — see the comment in imu.cpp).
	 */
	uint8_t reg;

	zassert_ok(emul_bmi270_get_reg(bmi_emul, REG_ACC_CONF, &reg, 1));
	zassert_equal(reg & ODR_MSK, ODR_25_HZ, "ACC_CONF ODR bits: 0x%02x", reg);

	zassert_ok(emul_bmi270_get_reg(bmi_emul, REG_GYR_CONF, &reg, 1));
	zassert_equal(reg & ODR_MSK, ODR_25_HZ, "GYR_CONF ODR bits: 0x%02x", reg);

	zassert_ok(emul_bmi270_get_reg(bmi_emul, REG_PWR_CTRL, &reg, 1));
	zassert_equal(reg & (PWR_CTRL_ACC_EN | PWR_CTRL_GYR_EN),
		      PWR_CTRL_ACC_EN | PWR_CTRL_GYR_EN,
		      "PWR_CTRL should enable accel+gyro: 0x%02x", reg);

	/* imu_init()'s sensor_trigger_set() routed data-ready to INT2 */
	zassert_ok(emul_bmi270_get_reg(bmi_emul, REG_INT_MAP_DATA, &reg, 1));
	zassert_equal(reg & INT_MAP_DATA_DRDY_INT2, INT_MAP_DATA_DRDY_INT2,
		      "INT_MAP_DATA: 0x%02x", reg);
}

ZTEST(imu_pipeline, test_sample_roundtrip)
{
	struct injected accel, gyro;
	struct imu_analysis_result frame1, frame2;

	inject_triple(SENSOR_CHAN_ACCEL_X, &accel);
	inject_triple(SENSOR_CHAN_GYRO_X, &gyro);

	fire_drdy();
	zassert_ok(k_msgq_get(&imu_result_q, &frame1, K_MSEC(500)),
		   "no frame published after DRDY");

	check_triple(&accel, &frame1.accel_x, "accel");
	check_triple(&gyro, &frame1.gyro_x, "gyro");

	/* A second frame proves the loop re-arms and seq is contiguous */
	fire_drdy();
	zassert_ok(k_msgq_get(&imu_result_q, &frame2, K_MSEC(500)),
		   "no second frame published");
	zassert_equal(frame2.seq, frame1.seq + 1, "seq %u then %u",
		      frame1.seq, frame2.seq);
}

ZTEST(imu_pipeline, test_queue_overflow_keeps_freshest)
{
	struct injected accel;
	struct imu_analysis_result frame;

	inject_triple(SENSOR_CHAN_ACCEL_X, &accel);

	/* Establish the current seq */
	fire_drdy();
	zassert_ok(k_msgq_get(&imu_result_q, &frame, K_MSEC(500)));

	uint32_t seq0 = frame.seq;

	/* Fire 6 frames without draining into the depth-4 queue: frames 1-4
	 * fill it, frame 5's put fails with -ENOMSG so imu_thread purges and
	 * re-puts, frame 6 lands behind it. Only frames 5 and 6 survive.
	 */
	for (int i = 0; i < 6; i++) {
		fire_drdy();
	}

	/* Let the last frame land before draining, so nothing leaks into the
	 * next test's queue.
	 */
	k_msleep(100);

	int drained = 0;
	uint32_t first_seq = 0;

	while (k_msgq_get(&imu_result_q, &frame, K_NO_WAIT) == 0) {
		if (drained == 0) {
			first_seq = frame.seq;
		}
		drained++;
	}

	zassert_equal(drained, 2, "expected 2 surviving frames, drained %d",
		      drained);
	zassert_equal(first_seq, seq0 + 5,
		      "oldest surviving frame: seq %u, expected %u",
		      first_seq, seq0 + 5);
	zassert_equal(frame.seq, seq0 + 6, "newest frame: seq %u, expected %u",
		      frame.seq, seq0 + 6);
}

ZTEST_SUITE(imu_pipeline, NULL, wait_for_imu_thread_startup, drain_queue, NULL, NULL);
