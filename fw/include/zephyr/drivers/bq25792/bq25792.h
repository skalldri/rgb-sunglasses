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

/**
 * @brief Read the system rail voltage from the VSYS_ADC register (REG3D).
 *
 * ADC must be enabled first via bq25792_adc_enable(). Unlike the legacy
 * bq25792_get_* getters, this propagates I2C errors instead of returning
 * stale data.
 *
 * @param dev     BQ25792 device pointer.
 * @param vsys_mv Output: VSYS voltage in millivolts.
 * @return 0 on success, negative errno on failure.
 */
int bq25792_get_vsys_mv(const struct device* dev, int32_t* vsys_mv);

/**
 * @brief Decoded snapshot of REG1B..REG1E (Charger_Status_0..3), fetched in a
 * single 4-byte burst read (one bridged I2Cm transaction).
 *
 * Field semantics per datasheet SLUSDG1C Tables 9-36..9-39.
 */
struct bq25792_status {
    /* REG1B_Charger_Status_0 */
    bool iindpm_active;   /**< In IINDPM/IOTG regulation — input current limited */
    bool vindpm_active;   /**< In VINDPM/VOTG regulation — input voltage sagging */
    bool wd_expired;      /**< I2C watchdog timer expired (registers reverted) */
    bool poor_source;     /**< Weak adapter detected */
    bool power_good;      /**< Input passed power-good qualification */
    bool vbus_present;    /**< VBUS above present threshold */
    /* REG1C_Charger_Status_1 */
    uint8_t chg_stat;     /**< 0 NotCharging..7 ChargeTerminationDone (3-bit) */
    uint8_t vbus_stat;    /**< Adapter type from BC1.2/detection (4-bit) */
    bool bc12_done;       /**< BC1.2 or non-standard detection complete */
    /* REG1D_Charger_Status_2 */
    uint8_t ico_stat;     /**< 0 disabled / 1 in progress / 2 max detected */
    bool thermal_regulation; /**< Device in thermal regulation (charge derated) */
    bool dpdm_ongoing;    /**< D+/D- detection currently running */
    bool vbat_present;    /**< Battery present (VBAT > VBAT_UVLOZ) */
    /* REG1E_Charger_Status_3 */
    bool adc_done;        /**< One-shot ADC conversion complete */
    bool vsysmin_regulation; /**< In VSYSMIN regulation (VBAT < VSYSMIN) */
    bool chg_timer_expired;     /**< Fast-charge safety timer expired */
    bool trickle_timer_expired; /**< Trickle-charge safety timer expired */
    bool precharge_timer_expired; /**< Pre-charge safety timer expired */
};

/**
 * @brief Read and decode Charger_Status_0..3 (REG1B..REG1E) in one burst.
 *
 * Propagates I2C errors (unlike the legacy getters) — on failure the output
 * struct is untouched.
 *
 * @param dev    BQ25792 device pointer.
 * @param status Output: decoded status flags.
 * @return 0 on success, negative errno on failure.
 */
int bq25792_get_status(const struct device* dev, struct bq25792_status* status);

/**
 * @brief Human-readable adapter type for bq25792_status::vbus_stat
 * (datasheet Table 9-37 VBUS_STAT encodings).
 */
const char* bq25792_vbus_stat_str(uint8_t vbus_stat);

/**
 * @brief Readback of the charger's current/voltage limit configuration.
 *
 * watchdog/vac_ovp hold the raw REG10 field encodings (Table 9-26):
 * watchdog 0h=disabled, 5h=40s (POR); vac_ovp 0h=26V, 1h=18V, 2h=12V, 3h=7V.
 */
struct bq25792_limits {
    uint32_t ichg_ma;     /**< REG03 charge current limit */
    uint32_t vindpm_mv;   /**< REG05 input voltage limit */
    uint32_t iindpm_ma;   /**< REG06 input current limit */
    uint32_t ico_ilim_ma; /**< REG19 ICO-discovered input limit (read-only) */
    uint8_t watchdog;     /**< REG10 WATCHDOG_2:0 raw encoding */
    uint8_t vac_ovp;      /**< REG10 VAC_OVP_1:0 raw encoding */
};

/**
 * @brief Read back ICHG/VINDPM/IINDPM/ICO/watchdog configuration.
 *
 * Five register reads (five bridged transactions). Propagates I2C errors —
 * on failure the output struct may be partially written but the call reports
 * the error.
 */
int bq25792_get_limits(const struct device* dev, struct bq25792_limits* limits);

/**
 * @brief Program the fast-charge current limit (ICHG, REG03).
 *
 * Clamped to the datasheet range 50..5000mA, 10mA resolution (values round
 * down). Writes then reads back and compares — returns -EIO on mismatch (the
 * TPS25750 host-interface TRM requires read-back verification of writes that
 * traverse the I2Cm bridge, since the PD controller's own event-driven writes
 * share the queue).
 *
 * NOTE: ICHG is reset to its POR default by the charger's I2C watchdog
 * (REG10) — disable or feed the watchdog or this setting reverts within 40s.
 *
 * @param dev     BQ25792 device pointer.
 * @param ichg_ma Requested charge current in mA.
 * @return 0 on success, -EIO on read-back mismatch, other negative errno on
 *         bus failure.
 */
int bq25792_set_charge_current_ma(const struct device* dev, uint32_t ichg_ma);

/**
 * @brief Program the input current limit (IINDPM, REG06).
 *
 * Clamped to the datasheet range 100..3300mA, 10mA resolution. Read-back
 * verified like bq25792_set_charge_current_ma(). Note the charger's own
 * BC1.2/adapter detection rewrites this register on every VBUS plug-in
 * (AUTO_INDET_EN default on) — callers must re-apply after plug events.
 */
int bq25792_set_input_current_limit_ma(const struct device* dev, uint32_t iindpm_ma);

/**
 * @brief Program the input voltage limit (VINDPM, REG05).
 *
 * Clamped to the datasheet range 3600..22000mV, 100mV resolution. Read-back
 * verified. The charger resets this to 3600mV on adapter unplug and re-derives
 * it from the VBUS measurement on plug-in — callers must re-apply after plug
 * events.
 */
int bq25792_set_input_voltage_limit_mv(const struct device* dev, uint32_t vindpm_mv);

/**
 * @brief Disable the charger's I2C watchdog (REG10 WATCHDOG_2:0 = 0h).
 *
 * The watchdog defaults to 40s and, on expiry, reverts all watchdog-scoped
 * registers (including ICHG and EN_CHG) to POR defaults. Read-back verified.
 */
int bq25792_watchdog_disable(const struct device* dev);

/**
 * @brief Feed the charger's I2C watchdog (REG10 WD_RST = 1).
 *
 * WD_RST self-clears after the timer resets, so this write is not read-back
 * verified.
 */
int bq25792_watchdog_feed(const struct device* dev);

/**
 * @brief Enter/leave input high-impedance (HIZ) mode (EN_HIZ, REG0F bit 2).
 *
 * With HIZ enabled the charger stops drawing from VBUS entirely and the
 * system runs from the battery — while the USB *data* connection stays up
 * (D+/D- don't route through the charger). Useful for draining the pack on
 * the bench with serial monitoring live. The part auto-clears EN_HIZ when an
 * adapter is (re)plugged at VBUS (datasheet SLUSDG1C Table 9-25), so a
 * physical replug always restores input power. Read-back verified.
 *
 * WARNING: enabling HIZ with no battery present removes the system's only
 * power source — callers must check battery presence first.
 */
int bq25792_hiz_enable(const struct device* dev, bool enable);

/**
 * @brief Program the VAC over-voltage protection threshold (REG10 VAC_OVP_1:0).
 *
 * Raw field encoding per datasheet SLUSDG1C Table 9-26: 0h=26V, 1h=18V,
 * 2h=12V, 3h=7V. Read-back verified. Values > 3h are rejected with -EINVAL.
 */
int bq25792_set_vac_ovp(const struct device* dev, uint8_t vac_ovp);

/**
 * @brief Enable/disable the Input Current Optimizer (EN_ICO, REG0F bit 4).
 *
 * With ICO enabled the effective input limit is the ICO result (REG19), not
 * the IINDPM register. Read-back verified.
 */
int bq25792_ico_enable(const struct device* dev, bool enable);

#ifdef __cplusplus
};
#endif