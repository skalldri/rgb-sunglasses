#pragma once

#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>

#ifdef __cplusplus
extern "C" {
#endif

int bq25792_dump(const struct device* dev);

int bq25792_temp_override(const struct device* dev, bool enable);

int bq25792_adc_enable(const struct device* dev, bool enable);

int bq25792_pfm_enable(const struct device* dev, bool enable);

typedef enum {
    HIGH = 0,
    LOW = 1,

    NUM_CHARGE_FREQUENCY
} bq25792_charge_frequency_t;

int bq25792_set_charge_frequency(const struct device* dev, bq25792_charge_frequency_t freq);

int bq25792_dump_charge_parameters(const struct device* dev);

int bq25792_set_charge_enable(const struct device* dev, bool enabled);

/**
 * @brief Read the CHG_STAT field from CHARGER_STATUS_1.
 *
 * @param dev      BQ25792 device pointer.
 * @param chg_stat Output: 3-bit charging status (0–7).
 * @return 0 on success, negative errno on failure.
 */
int bq25792_get_charge_status(const struct device* dev, uint8_t* chg_stat);

#ifdef __cplusplus
};
#endif