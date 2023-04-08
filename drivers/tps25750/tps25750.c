#include <zephyr/init.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/tps25750/tps25750.h>
#include "tps25750_priv.h"

#define DT_DRV_COMPAT ti_tps25750

LOG_MODULE_REGISTER(tps25750, LOG_LEVEL_INF);

/**
 * @brief Construct a byte array that can be sent to one of the INT registers based on
 * a tps25750_int_t
 * 
 * @param i 
 * @param bytes 
 */
void _tps25750_int_to_bytes(const tps25750_int_t* i, uint8_t bytes[TPS25750_REG_INT_SIZE]) {
    #define TPS25750_INT_BIT(_name, _byte, _bit) \
        bytes[_byte] = (bytes[_byte] & (~(1 << _bit))) | (i->_name << _bit);

    TPS25750_INT_BIT_LIST
    #undef TPS25750_INT_BIT
}

/**
 * @brief Convert raw data from one of the INT registers into the structured tps25750_int_t
 * 
 * @param bytes 
 * @param i 
 */
void _bytes_to_tps25750_int(const uint8_t bytes[TPS25750_REG_INT_SIZE], tps25750_int_t* i) {
    #define TPS25750_INT_BIT(_name, _byte, _bit) \
        i->_name = tps25750_int_bit_##_name(bytes);

    TPS25750_INT_BIT_LIST
    #undef TPS25750_INT_BIT
}

int tps25750_read_int_event1(const struct device *dev, tps25750_int_t* i) {
    if (!dev || !i) {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    uint8_t bytes[TPS25750_REG_INT_SIZE];

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    int ret = i2c_burst_read_dt(
        &cfg->i2c, 
        TPS25750_REG_INT_EVENT1_ADDR, 
        bytes, 
        sizeof(bytes));
    
    if (ret) {
        return ret;
    }
    
    _bytes_to_tps25750_int(bytes, i);

    LOG_HEXDUMP_INF(bytes, sizeof(bytes), "INT_EVENT1");

    return 0;
}

int tps25750_read_int_mask1(const struct device *dev, tps25750_int_t* i) {
    if (!dev || !i) {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    uint8_t bytes[TPS25750_REG_INT_SIZE];

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    int ret = i2c_burst_read_dt(
        &cfg->i2c, 
        TPS25750_REG_INT_MASK1_ADDR, 
        bytes, 
        sizeof(bytes));
    
    if (ret) {
        return ret;
    }
    
    _bytes_to_tps25750_int(bytes, i);

    LOG_HEXDUMP_INF(bytes, sizeof(bytes), "INT_MASK1");

    return 0;
}

int tps25750_read_mode(const struct device *dev, tps25750_mode_t* mode) {
    if (!dev || !mode) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_read_dt(
        &cfg->i2c, 
        TPS25750_REG_MODE_ADDR, 
        mode->mode, 
        sizeof(mode->mode));
}

int tps25750_read_cmd1(const struct device *dev, tps25750_cmd1_t* cmd) {
    if (!dev || !cmd) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_read_dt(
        &cfg->i2c, 
        TPS25750_REG_CMD1_ADDR, 
        cmd->cmd, 
        sizeof(cmd->cmd));
}

int tps25750_write_cmd1(const struct device *dev, tps25750_cmd1_t* cmd) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_write_dt(
        &cfg->i2c, 
        TPS25750_REG_CMD1_ADDR, 
        cmd->cmd, 
        sizeof(cmd->cmd));
}

int tps25750_dump(const struct device *dev)
{
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;
    int ret;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    tps25750_mode_t mode;
    ret = tps25750_read_mode(dev, &mode);
    if (ret) {
        LOG_ERR("tps25750_read_mode: %d", ret);
        return ret;
    }
    LOG_INF("MODE: %.*s", sizeof(mode.mode), mode.mode);

    tps25750_int_t i;
    ret = tps25750_read_int_event1(dev, &i);
    if (ret) {
        LOG_ERR("tps25750_read_int_event1: %d", ret);
        return ret;
    }

    // Use X-macro magic to make dumping the gigantic INT_EVENT1 register less painful
    #define TPS25750_INT_BIT(_name, _byte, _bit) \
        LOG_INF("EVENT %s: %d", #_name, i._name);

    TPS25750_INT_BIT_LIST
    #undef TPS25750_INT_BIT

    ret = tps25750_read_int_mask1(dev, &i);
    if (ret) {
        LOG_ERR("tps25750_read_int_mask1: %d", ret);
        return ret;
    }

    // Use X-macro magic to make dumping the gigantic INT_EVENT1 register less painful
    #define TPS25750_INT_BIT(_name, _byte, _bit) \
        LOG_INF("MASK %s: %d", #_name, i._name);

    TPS25750_INT_BIT_LIST
    #undef TPS25750_INT_BIT

    LOG_INF("Dump complete!");

    return 0;
}

static int tps25750_init(const struct device *dev)
{
    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return 0;
}

#define TPS25750_DEFINE(inst)                                             \
    static struct tps25750_dev_data tps25750_data_##inst;                 \
                                                                          \
    static const struct tps25750_dev_config tps25750_config_##inst =      \
        {                                                                 \
            .i2c = I2C_DT_SPEC_INST_GET(inst),                            \
    };                                                                    \
                                                                          \
    DEVICE_DT_INST_DEFINE(inst, tps25750_init, NULL,                      \
                          &tps25750_data_##inst, &tps25750_config_##inst, \
                          POST_KERNEL, CONFIG_TPS25750_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(TPS25750_DEFINE)