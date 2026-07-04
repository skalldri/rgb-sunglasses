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
 * @brief Callback type invoked from the BQ25792 interrupt handler.
 *
 * Called in ISR context — keep it short; no I2C or blocking calls.
 */
typedef void (*bq25792_irq_callback_t)(const struct device *dev, void *user_data);

/**
 * @brief Register a callback to be invoked when the BQ25792 INT pin fires.
 *
 * Only one callback may be registered at a time; calling this again replaces
 * the previous registration. Pass NULL to clear the callback.
 *
 * @param dev       BQ25792 device pointer.
 * @param cb        Callback function, or NULL.
 * @param user_data Opaque pointer passed through to @p cb.
 * @return 0 on success, -ENODEV if @p dev is NULL.
 */
int bq25792_register_irq_callback(const struct device *dev, bq25792_irq_callback_t cb,
                                   void *user_data);

/**
 * @brief Read the CHG_STAT field from CHARGER_STATUS_1.
 *
 * @param dev      BQ25792 device pointer.
 * @param chg_stat Output: 3-bit charging status (0–7).
 * @return 0 on success, negative errno on failure.
 */
int bq25792_get_charge_status(const struct device* dev, uint8_t* chg_stat);

/**
 * @brief Read the battery voltage from the VBAT_ADC register.
 *
 * ADC must be enabled first via bq25792_adc_enable().
 *
 * @param dev     BQ25792 device pointer.
 * @param vbat_mv Output: battery voltage in millivolts.
 * @return 0 on success, negative errno on failure.
 */
int bq25792_get_vbat_mv(const struct device* dev, int32_t* vbat_mv);

/**
 * @brief Read back the EN_CHG bit from CHARGER_CONTROL_0.
 *
 * @param dev     BQ25792 device pointer.
 * @param enabled Output: true if battery charging is enabled.
 * @return 0 on success, negative errno on failure.
 */
int bq25792_get_charge_enable(const struct device* dev, bool* enabled);

/**
 * @brief Enable or disable battery discharge-current sensing (EN_IBAT).
 *
 * EN_IBAT defaults to 0 at reset; without it the IBAT_ADC register reads 0
 * while the battery is discharging. Required for bq25792_get_ibat_ma() to
 * report discharge current.
 *
 * @param dev    BQ25792 device pointer.
 * @param enable true to enable IBAT sensing.
 * @return 0 on success, negative errno on failure.
 */
int bq25792_ibat_sense_enable(const struct device* dev, bool enable);

/**
 * @brief Read the battery current from the IBAT_ADC register.
 *
 * ADC must be enabled first via bq25792_adc_enable(), and IBAT sensing via
 * bq25792_ibat_sense_enable(). Positive = charging (current into the
 * battery), negative = discharging.
 *
 * @param dev     BQ25792 device pointer.
 * @param ibat_ma Output: battery current in milliamps (signed).
 * @return 0 on success, negative errno on failure.
 */
int bq25792_get_ibat_ma(const struct device* dev, int32_t* ibat_ma);

/**
 * @brief Read the USB input voltage from the VBUS_ADC register.
 *
 * ADC must be enabled first via bq25792_adc_enable().
 *
 * @param dev     BQ25792 device pointer.
 * @param vbus_mv Output: VBUS voltage in millivolts.
 * @return 0 on success, negative errno on failure.
 */
int bq25792_get_vbus_mv(const struct device* dev, int32_t* vbus_mv);

/**
 * @brief Read the USB input current from the IBUS_ADC register.
 *
 * ADC must be enabled first via bq25792_adc_enable(). Positive = current
 * flowing in from VBUS, negative = reverse (OTG) mode.
 *
 * @param dev     BQ25792 device pointer.
 * @param ibus_ma Output: VBUS input current in milliamps (signed).
 * @return 0 on success, negative errno on failure.
 */
int bq25792_get_ibus_ma(const struct device* dev, int32_t* ibus_ma);

#ifdef __cplusplus
};
#endif