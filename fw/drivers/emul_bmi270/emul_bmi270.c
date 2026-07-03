/*
 * Emulator for the Bosch BMI270 accelerometer/gyro, SPI bus only (the only
 * bus this project uses — see the bmi270 node on &spi3 in the proto0 DTS).
 *
 * Upstream Zephyr has no BMI270 emulator, and the NCS tree cannot be
 * modified, so this lives out-of-tree. It emulates exactly what the real
 * driver (zephyr/drivers/sensor/bosch/bmi270/) does on the bus:
 *
 *  - init handshake: chip-id read (0x24), soft reset, PWR_CONF read-modify-
 *    write, INIT_CTRL 0 -> config-file upload -> INIT_CTRL 1, then the
 *    INTERNAL_STATUS poll must return INIT_OK.
 *  - sampling: one 12-byte burst read from ACC_X_LSB (accel XYZ then gyro
 *    XYZ, little-endian int16).
 *  - attrs: ACC_CONF/ACC_RANGE/GYR_CONF/GYR_RANGE/PWR_CTRL writes land in
 *    the register file so tests can assert on them.
 *
 * Tests inject samples through the sensor emulator backend API
 * (emul_sensor_backend_set_channel), which converts SI q31 values to raw
 * LSBs honoring the currently-programmed range registers — the same math
 * the driver applies in reverse in channel_get.
 *
 * The BMI270's SPI framing is asymmetric and differs from the BMI160 emul:
 *  - read:  tx count 1 (addr|0x80); rx count 2, rx[0].len==2 (addr echo +
 *           dummy byte, discarded by the driver), rx[1] = payload.
 *  - write: tx count 2 (addr & 0x7F, then payload burst); no rx at all.
 * See bmi270_spi.c (bmi270_reg_read_spi/bmi270_reg_write_spi).
 */

#define DT_DRV_COMPAT bosch_bmi270

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(emul_bmi270, CONFIG_SENSOR_LOG_LEVEL);

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/emul_sensor.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi_emul.h>
#include <zephyr/sys/util.h>

#include "emul_bmi270.h"

/* Register addresses and values, mirroring the (non-exported) defines in
 * zephyr/drivers/sensor/bosch/bmi270/bmi270.h. Only what the emul needs.
 */
#define EMUL_BMI270_REG_CHIP_ID         0x00
#define EMUL_BMI270_REG_ACC_X_LSB       0x0C
#define EMUL_BMI270_REG_GYR_X_LSB       0x12
#define EMUL_BMI270_REG_INTERNAL_STATUS 0x21
#define EMUL_BMI270_REG_ACC_RANGE       0x41
#define EMUL_BMI270_REG_GYR_RANGE       0x43
#define EMUL_BMI270_REG_INIT_CTRL       0x59
#define EMUL_BMI270_REG_INIT_DATA       0x5E
#define EMUL_BMI270_REG_PWR_CONF        0x7C
#define EMUL_BMI270_REG_CMD             0x7E
#define EMUL_BMI270_REG_COUNT           0x80

#define EMUL_BMI270_CHIP_ID              0x24
#define EMUL_BMI270_CMD_SOFT_RESET       0xB6
#define EMUL_BMI270_COMPLETE_CONFIG_LOAD 0x01
#define EMUL_BMI270_INST_MESSAGE_INIT_OK 0x01

/* Real-chip power-on default: advanced power save enabled. The driver
 * read-modify-writes this during init; the value itself doesn't matter to
 * the emul, but starting from the true default keeps traces realistic.
 */
#define EMUL_BMI270_PWR_CONF_DEFAULT 0x03

/* Range-register default values matching the driver's own cached defaults
 * set in bmi270_init() (acc_range = 8 g, gyr_range = 2000 dps). Keeping the
 * register file and the driver cache in sync is what makes backend
 * injection scale correctly before any SENSOR_ATTR_FULL_SCALE call.
 */
#define EMUL_BMI270_ACC_RANGE_DEFAULT 0x02 /* BMI270_ACC_RANGE_8G */
#define EMUL_BMI270_GYR_RANGE_DEFAULT 0x00 /* BMI270_GYR_RANGE_2000DPS */

/* Micro-SI conversion constants, matching the driver's channel_*_convert():
 * SENSOR_G = 9806650 u(m/s^2)/g and SENSOR_PI = 3141592 urad.
 */
#define EMUL_BMI270_UG  9806650LL
#define EMUL_BMI270_UPI 3141592LL

struct emul_bmi270_data {
	uint8_t regs[EMUL_BMI270_REG_COUNT];
	/* Bytes burst-written to INIT_DATA since last reset (config upload) */
	size_t cfg_bytes_written;
};

static void emul_bmi270_reset(struct emul_bmi270_data *data)
{
	memset(data->regs, 0, sizeof(data->regs));
	data->regs[EMUL_BMI270_REG_CHIP_ID] = EMUL_BMI270_CHIP_ID;
	data->regs[EMUL_BMI270_REG_PWR_CONF] = EMUL_BMI270_PWR_CONF_DEFAULT;
	data->regs[EMUL_BMI270_REG_ACC_RANGE] = EMUL_BMI270_ACC_RANGE_DEFAULT;
	data->regs[EMUL_BMI270_REG_GYR_RANGE] = EMUL_BMI270_GYR_RANGE_DEFAULT;
	data->cfg_bytes_written = 0;
}

static void reg_write(const struct emul *target, uint8_t regn, uint8_t val)
{
	struct emul_bmi270_data *data = target->data;

	LOG_DBG("write 0x%02x = 0x%02x", regn, val);

	switch (regn) {
	case EMUL_BMI270_REG_CMD:
		if (val == EMUL_BMI270_CMD_SOFT_RESET) {
			LOG_DBG("   * soft reset");
			emul_bmi270_reset(data);
		}
		break;
	case EMUL_BMI270_REG_INIT_CTRL:
		data->regs[regn] = val;
		/* Completing the config load flips the internal status to
		 * INIT_OK — but only if a config file was actually uploaded,
		 * so a broken upload path fails init just like real hardware.
		 */
		if (val == EMUL_BMI270_COMPLETE_CONFIG_LOAD &&
		    data->cfg_bytes_written > 0) {
			data->regs[EMUL_BMI270_REG_INTERNAL_STATUS] =
				EMUL_BMI270_INST_MESSAGE_INIT_OK;
		}
		break;
	default:
		data->regs[regn] = val;
		break;
	}
}

static int emul_bmi270_io_spi(const struct emul *target, const struct spi_config *config,
			      const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs)
{
	struct emul_bmi270_data *data = target->data;
	const struct spi_buf *tx;
	uint8_t addr;

	ARG_UNUSED(config);

	if (tx_bufs == NULL || tx_bufs->count < 1) {
		LOG_ERR("no tx buffer");
		return -EIO;
	}

	tx = &tx_bufs->buffers[0];
	if (tx->len != 1) {
		LOG_ERR("unexpected tx[0] len %zu", (size_t)tx->len);
		return -EIO;
	}
	addr = *(uint8_t *)tx->buf;

	if (addr & 0x80) {
		/* Register read: rx[0] is the discard buffer (addr echo +
		 * dummy byte), rx[1] receives the payload with the register
		 * address auto-incrementing (covers the 12-byte sample burst).
		 */
		const struct spi_buf *rxd;
		uint8_t regn = addr & 0x7F;

		if (rx_bufs == NULL || rx_bufs->count != 2 ||
		    rx_bufs->buffers[0].len != 2) {
			LOG_ERR("unexpected read framing");
			return -EIO;
		}

		memset(rx_bufs->buffers[0].buf, 0, rx_bufs->buffers[0].len);
		rxd = &rx_bufs->buffers[1];
		for (size_t i = 0; i < rxd->len; i++) {
			uint8_t r = regn + i;

			((uint8_t *)rxd->buf)[i] =
				(r < EMUL_BMI270_REG_COUNT) ? data->regs[r] : 0;
		}
		LOG_DBG("read 0x%02x len %zu = 0x%02x ...", regn, (size_t)rxd->len,
			((uint8_t *)rxd->buf)[0]);
	} else {
		/* Register write: tx[1] is the payload burst */
		const struct spi_buf *txd;

		if (tx_bufs->count != 2 || rx_bufs != NULL) {
			LOG_ERR("unexpected write framing");
			return -EIO;
		}

		txd = &tx_bufs->buffers[1];
		if (addr == EMUL_BMI270_REG_INIT_DATA) {
			/* Config-file chunk: 256 bytes per burst — counted,
			 * not stored (it would overflow the register file and
			 * the driver never reads it back).
			 */
			data->cfg_bytes_written += txd->len;
			LOG_DBG("config chunk of %zu bytes (total %zu)",
				(size_t)txd->len, data->cfg_bytes_written);
		} else {
			for (size_t i = 0; i < txd->len; i++) {
				uint8_t r = addr + i;

				if (r < EMUL_BMI270_REG_COUNT) {
					reg_write(target, r, ((uint8_t *)txd->buf)[i]);
				}
			}
		}
	}

	return 0;
}

static struct spi_emul_api emul_bmi270_api_spi = {
	.io = emul_bmi270_io_spi,
};

/* Full scale of the currently-programmed accel range, in micro-m/s^2 */
static int64_t acc_full_scale_usi(const struct emul_bmi270_data *data)
{
	uint8_t bits = data->regs[EMUL_BMI270_REG_ACC_RANGE] & 0x03;
	/* 0 -> 2 g, 1 -> 4 g, 2 -> 8 g, 3 -> 16 g */
	int64_t range_g = 2 << bits;

	return range_g * EMUL_BMI270_UG;
}

/* Full scale of the currently-programmed gyro range, in dps (not micro) */
static int64_t gyr_full_scale_dps(const struct emul_bmi270_data *data)
{
	switch (data->regs[EMUL_BMI270_REG_GYR_RANGE] & 0x07) {
	case 0x00:
		return 2000;
	case 0x01:
		return 1000;
	case 0x02:
		return 500;
	case 0x03:
		return 250;
	case 0x04:
		return 125;
	default:
		return 0;
	}
}

static int emul_bmi270_backend_set_channel(const struct emul *target, struct sensor_chan_spec ch,
					   const q31_t *value, int8_t shift)
{
	struct emul_bmi270_data *data = target->data;
	int64_t si_micro;
	int64_t raw;
	int reg_lsb;

	if (value == NULL || ch.chan_idx != 0) {
		return -EINVAL;
	}

	/* SI value in micro-units (micro-m/s^2 or micro-rad/s). The q31
	 * value represents value * 2^shift / 2^31.
	 */
	si_micro = ((int64_t)*value * 1000000LL) >> (31 - shift);

	switch (ch.chan_type) {
	case SENSOR_CHAN_ACCEL_X:
	case SENSOR_CHAN_ACCEL_Y:
	case SENSOR_CHAN_ACCEL_Z:
		reg_lsb = EMUL_BMI270_REG_ACC_X_LSB +
			  (ch.chan_type - SENSOR_CHAN_ACCEL_X) * 2;
		/* Inverse of the driver's channel_accel_convert():
		 * si_micro = raw * SENSOR_G * range_g / INT16_MAX
		 */
		raw = si_micro * INT16_MAX / acc_full_scale_usi(data);
		break;
	case SENSOR_CHAN_GYRO_X:
	case SENSOR_CHAN_GYRO_Y:
	case SENSOR_CHAN_GYRO_Z: {
		int64_t range_dps = gyr_full_scale_dps(data);

		if (range_dps == 0) {
			return -EINVAL;
		}
		reg_lsb = EMUL_BMI270_REG_GYR_X_LSB +
			  (ch.chan_type - SENSOR_CHAN_GYRO_X) * 2;
		/* Inverse of the driver's channel_gyro_convert():
		 * si_micro = raw * range_dps * SENSOR_PI / (180 * INT16_MAX)
		 */
		raw = si_micro * INT16_MAX * 180LL / (range_dps * EMUL_BMI270_UPI);
		break;
	}
	default:
		return -ENOTSUP;
	}

	raw = CLAMP(raw, INT16_MIN, INT16_MAX);
	data->regs[reg_lsb] = (uint8_t)(raw & 0xFF);
	data->regs[reg_lsb + 1] = (uint8_t)((raw >> 8) & 0xFF);

	return 0;
}

static int emul_bmi270_backend_get_sample_range(const struct emul *target,
						struct sensor_chan_spec ch, q31_t *lower,
						q31_t *upper, q31_t *epsilon, int8_t *shift)
{
	struct emul_bmi270_data *data = target->data;
	int64_t fs_micro;
	int8_t s;

	if (ch.chan_idx != 0) {
		return -EINVAL;
	}

	switch (ch.chan_type) {
	case SENSOR_CHAN_ACCEL_X:
	case SENSOR_CHAN_ACCEL_Y:
	case SENSOR_CHAN_ACCEL_Z:
	case SENSOR_CHAN_ACCEL_XYZ:
		fs_micro = acc_full_scale_usi(data);
		/* 2 g = 19.6 -> shift 5 ... 16 g = 156.9 -> shift 8 */
		s = 5 + (data->regs[EMUL_BMI270_REG_ACC_RANGE] & 0x03);
		break;
	case SENSOR_CHAN_GYRO_X:
	case SENSOR_CHAN_GYRO_Y:
	case SENSOR_CHAN_GYRO_Z:
	case SENSOR_CHAN_GYRO_XYZ: {
		int64_t range_dps = gyr_full_scale_dps(data);

		if (range_dps == 0) {
			return -EINVAL;
		}
		fs_micro = range_dps * EMUL_BMI270_UPI / 180LL;
		/* 125 dps = 2.18 rad/s -> shift 2 ... 2000 dps = 34.9 -> shift 6 */
		s = 6 - (data->regs[EMUL_BMI270_REG_GYR_RANGE] & 0x07);
		break;
	}
	default:
		return -ENOTSUP;
	}

	*shift = s;
	*upper = (q31_t)(fs_micro * (1LL << (31 - s)) / 1000000LL);
	*lower = -*upper;
	/* A few LSBs of tolerance: one from the emul's SI->raw rounding, one
	 * from the driver's raw->SI conversion, plus margin.
	 */
	*epsilon = (q31_t)(*upper * 4LL / INT16_MAX) + 1;

	return 0;
}

static const struct emul_sensor_driver_api emul_bmi270_backend_api = {
	.set_channel = emul_bmi270_backend_set_channel,
	.get_sample_range = emul_bmi270_backend_get_sample_range,
};

int emul_bmi270_get_reg(const struct emul *target, uint8_t reg, uint8_t *out, size_t count)
{
	struct emul_bmi270_data *data = target->data;

	if (reg + count > EMUL_BMI270_REG_COUNT) {
		return -EINVAL;
	}
	memcpy(out, &data->regs[reg], count);
	return 0;
}

int emul_bmi270_set_reg(const struct emul *target, uint8_t reg, const uint8_t *in, size_t count)
{
	struct emul_bmi270_data *data = target->data;

	if (reg + count > EMUL_BMI270_REG_COUNT) {
		return -EINVAL;
	}
	memcpy(&data->regs[reg], in, count);
	return 0;
}

size_t emul_bmi270_cfg_bytes_written(const struct emul *target)
{
	struct emul_bmi270_data *data = target->data;

	return data->cfg_bytes_written;
}

static int emul_bmi270_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(parent);

	emul_bmi270_reset(target->data);
	return 0;
}

#define EMUL_BMI270_DEFINE(n)                                                                      \
	static struct emul_bmi270_data emul_bmi270_data_##n;                                       \
	EMUL_DT_INST_DEFINE(n, emul_bmi270_init, &emul_bmi270_data_##n, NULL,                      \
			    &emul_bmi270_api_spi, &emul_bmi270_backend_api)

DT_INST_FOREACH_STATUS_OKAY(EMUL_BMI270_DEFINE)
