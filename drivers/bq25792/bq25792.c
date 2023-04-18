#include <zephyr/init.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/bq25792/bq25792.h>
#include "bq25792_priv.h"

#define DT_DRV_COMPAT ti_bq25792

LOG_MODULE_REGISTER(bq25792, LOG_LEVEL_DBG);

#define BIT_EXTRACT(_val, _start, _num) (((_val) & (((1 << _num) - 1) << _start)) >> _start)

#define BQ25792_BIT(_name, _bitLowest, _numBits) \
    dst->_name = BIT_EXTRACT(r, _bitLowest, _numBits); \
    LOG_DBG(#_name ": %u", dst->_name); 

#define REG(_regName) \
    int bq25792_read_##_regName(const struct device *dev, _regName##_t* dst) { \
        const struct bq25792_dev_config *cfg = dev->config; \
        uint8_t r; \
        int ret = i2c_burst_read_dt( \
        &cfg->i2c, \
        _regName##_ADDR, \
        (uint8_t *)&r, \
        sizeof(r)); \
        if (ret) {   \
            LOG_ERR("I2C Read failed: %d", ret); \
            return ret; \
        } \
        LOG_DBG("Reading " #_regName ": %u", r); \
        _regName##_BITS \
        return 0; \
    }

REG_LIST

#undef REG

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

    BQ25792_REG_CHARGER_STATUS_0_t status0;
    int ret = bq25792_read_BQ25792_REG_CHARGER_STATUS_0(dev, &status0);

    BQ25792_REG_CHARGER_STATUS_1_t status1;
    ret = bq25792_read_BQ25792_REG_CHARGER_STATUS_1(dev, &status1);

    BQ25792_REG_CHARGER_STATUS_2_t status2;
    ret = bq25792_read_BQ25792_REG_CHARGER_STATUS_2(dev, &status2);

    BQ25792_REG_CHARGER_STATUS_3_t status3;
    ret = bq25792_read_BQ25792_REG_CHARGER_STATUS_3(dev, &status3);

    BQ25792_REG_CHARGER_STATUS_4_t status4;
    ret = bq25792_read_BQ25792_REG_CHARGER_STATUS_4(dev, &status4);

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

    BQ25792_REG_FAULT_STATUS_1_t reg;
    bq25792_read_BQ25792_REG_FAULT_STATUS_1(dev, &reg);

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