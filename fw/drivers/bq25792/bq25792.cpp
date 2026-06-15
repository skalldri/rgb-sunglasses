#include <zephyr/drivers/bq25792/bq25792.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "bq25792_init.h"
#include "bq25792_irq.h"

LOG_MODULE_REGISTER(bq25792, LOG_LEVEL_INF);

// Must be included after LOG_MODULE_REGISTER() since this contains LOG_XXX statements
// TODO: fix this!
#include "bq25792_priv.h"

/* X-Macro Definition for bq25792_dump() function */
#define REG(_regName)      \
    {                      \
        _regName tmp(cfg); \
        tmp.dump();        \
    }

int bq25792_dump(const struct device *dev) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config *)dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

#if defined(CONFIG_DUMP_DEVICE_REGISTERS)

    REG_LIST

#endif

    return 0;
}
#undef REG

int bq25792_temp_override(const struct device *dev, bool enable) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config *)dev->config;

    BQ25792_NTC_CONTROL_1 reg(cfg);

    uint32_t ts_ignore = enable ? 1 : 0;
    LOG_INF("Setting TS_IGNORE to %u", ts_ignore);

    return reg.set<BQ25792_NTC_CONTROL_1_TS_IGNORE>(ts_ignore, true /* flush */);
}

int bq25792_adc_enable(const struct device *dev, bool enable) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config *)dev->config;

    BQ25792_ADC_CONTROL reg(cfg);

    uint32_t adc_en = enable ? 1 : 0;
    LOG_INF("Setting ADC_EN to %u", adc_en);

    return reg.set<BQ25792_ADC_CONTROL_ADC_EN>(adc_en, true /* flush */);
}

int bq25792_pfm_enable(const struct device *dev, bool enable) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config *)dev->config;

    BQ25792_CHARGER_CONTROL_3 reg(cfg);

    uint32_t pfm_fwd_dis = enable ? 0 : 1;
    LOG_INF("Setting PFM_FWD_DIS to %u", pfm_fwd_dis);

    return reg.set<BQ25792_CHARGER_CONTROL_3_PFM_FWD_DIS>(pfm_fwd_dis, true /* flush */);
}

int bq25792_set_charge_frequency(const struct device *dev, bq25792_charge_frequency_t freq) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    if (freq > NUM_CHARGE_FREQUENCY) {
        LOG_ERR("Invalid frequency setting %u", freq);
        return -EINVAL;
    }

    const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config *)dev->config;

    BQ25792_CHARGER_CONTROL_4 reg(cfg);

    uint32_t pwm_freq = (freq == bq25792_charge_frequency_t::LOW) ? 1 : 0;
    LOG_INF("Setting PWM_FREQ to %u", pwm_freq);

    return reg.set<BQ25792_CHARGER_CONTROL_4_PWM_FREQ>(pwm_freq, true /* flush */);
}

int bq25792_dump_charge_parameters(const struct device *dev) {
    const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config *)dev->config;

    BQ25792_MINIMAL_SYSTEM_VOLTAGE minimalSystemVoltage(cfg);
    BQ25792_RECHARGE_CONTROL rechargeControl(cfg);
    BQ25792_CHARGE_VOLTAGE_LIMIT chargeVoltageLimit(cfg);
    BQ25792_VBAT_ADC vbatAdc(cfg);

    BQ25792_CHARGER_CONTROL_0 chargerControl0(cfg);

    BQ25792_CHARGER_STATUS_0 chargerStatus0(cfg);
    BQ25792_CHARGER_STATUS_1 chargerStatus1(cfg);
    BQ25792_CHARGER_STATUS_2 chargerStatus2(cfg);
    BQ25792_CHARGER_STATUS_3 chargerStatus3(cfg);
    BQ25792_CHARGER_STATUS_4 chargerStatus4(cfg);

    BQ25792_FAULT_STATUS_0 faultStatus0(cfg);
    BQ25792_FAULT_STATUS_1 faultStatus1(cfg);

    minimalSystemVoltage.dump();
    rechargeControl.dump();
    chargeVoltageLimit.dump();
    vbatAdc.dump();
    chargerControl0.dump();
    chargerStatus0.dump();
    chargerStatus1.dump();
    chargerStatus2.dump();
    chargerStatus3.dump();
    chargerStatus4.dump();
    faultStatus0.dump();
    faultStatus1.dump();

    return 0;
}

int bq25792_get_charge_status(const struct device *dev, uint8_t *chg_stat) {
    if (!dev || !chg_stat) {
        return -EINVAL;
    }
    const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config *)dev->config;
    BQ25792_CHARGER_STATUS_1 reg(cfg);
    *chg_stat = (uint8_t)reg.get<BQ25792_CHARGER_STATUS_1_CHG_STAT>(0, true /* read from hw */);
    return 0;
}

int bq25792_set_charge_enable(const struct device *dev, bool enabled) {
    const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config *)dev->config;
    BQ25792_CHARGER_CONTROL_0 chargerControl0(cfg);
    LOG_INF("Setting EN_CHG to %u", enabled ? 1 : 0);
    chargerControl0.set<BQ25792_CHARGER_CONTROL_0_EN_CHG>(enabled ? 1 : 0, true /* flush */);
    return 0;
}

int bq25792_register_irq_callback(const struct device *dev, bq25792_irq_callback_t cb,
                                   void *user_data) {
    if (!dev) {
        return -ENODEV;
    }

    struct bq25792_dev_data *data = (struct bq25792_dev_data *)dev->data;
    data->irq_cb = cb;
    data->irq_cb_user_data = user_data;
    return 0;
}

void bq25792_irq(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins) {
    struct bq25792_dev_data *data = CONTAINER_OF(cb, struct bq25792_dev_data, callback);

    LOG_INF("BQ25792 interrupt received!");

    if (data->irq_cb) {
        data->irq_cb(data->dev, data->irq_cb_user_data);
    }
}