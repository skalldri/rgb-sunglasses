#include <zephyr/init.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/bq25792/bq25792.h>
#include "bq25792_priv.h"

#define DT_DRV_COMPAT ti_bq25792

LOG_MODULE_REGISTER(bq25792, LOG_LEVEL_INF);

int bq25792_read_charger_status(const struct device *dev) {
    if (!dev)
    {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct bq25792_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    uint8_t b;

    int ret = i2c_burst_read_dt(
        &cfg->i2c,
        BQ25792_REG_CHARGER_STATUS_0_ADDR,
        (uint8_t *)&b,
        sizeof(b));
    if (ret) {
        LOG_ERR("I2C Read failed: %d", ret);
        return ret;
    }
    LOG_INF("Charger Status 0: %u", b);

    ret = i2c_burst_read_dt(
        &cfg->i2c,
        BQ25792_REG_CHARGER_STATUS_1_ADDR,
        (uint8_t *)&b,
        sizeof(b));
    if (ret) {
        LOG_ERR("I2C Read failed: %d", ret);
        return ret;
    }
    LOG_INF("Charger Status 1: %u", b);

    ret = i2c_burst_read_dt(
        &cfg->i2c,
        BQ25792_REG_CHARGER_STATUS_2_ADDR,
        (uint8_t *)&b,
        sizeof(b));
    if (ret) {
        LOG_ERR("I2C Read failed: %d", ret);
        return ret;
    }
    LOG_INF("Charger Status 2: %u", b);

    ret = i2c_burst_read_dt(
        &cfg->i2c,
        BQ25792_REG_CHARGER_STATUS_3_ADDR,
        (uint8_t *)&b,
        sizeof(b));
    if (ret) {
        LOG_ERR("I2C Read failed: %d", ret);
        return ret;
    }
    LOG_INF("Charger Status 3: %u", b);

    ret = i2c_burst_read_dt(
        &cfg->i2c,
        BQ25792_REG_CHARGER_STATUS_4_ADDR,
        (uint8_t *)&b,
        sizeof(b));
    if (ret) {
        LOG_ERR("I2C Read failed: %d", ret);
        return ret;
    }
    LOG_INF("Charger Status 4: %u", b);

    return 0;
}

int bq25792_dump(const struct device *dev)
{
    if (!dev)
    {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct bq25792_dev_config *cfg = dev->config;
    int ret;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    ret = bq25792_read_charger_status(dev);

    return 0;
}

static int bq25792_init(const struct device *dev) {
    return 0;
}

#define BQ25792_DEFINE(inst)                                                           \
    static struct bq25792_dev_data bq25792_data_##inst;                                \
                                                                                       \
                                                                                       \
    static const struct bq25792_dev_config bq25792_config_##inst =                     \
        {                                                                              \
            .i2c = I2C_DT_SPEC_INST_GET(inst)                                          \
        };                                                                             \
                                                                                       \
    DEVICE_DT_INST_DEFINE(inst, bq25792_init, NULL,                                    \
                      &bq25792_data_##inst, &bq25792_config_##inst,                    \
                      APPLICATION, CONFIG_BQ25792_INIT_PRIORITY, NULL);
    

DT_INST_FOREACH_STATUS_OKAY(BQ25792_DEFINE)