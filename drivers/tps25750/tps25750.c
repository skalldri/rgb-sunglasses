#include <zephyr/init.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/tps25750/tps25750.h>
#include "tps25750_priv.h"

#define DT_DRV_COMPAT ti_tps25750

LOG_MODULE_REGISTER(tps25750);

int tps25750_dump(const struct device *dev)
{
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    struct tps25750_dev_data *data = dev->data;
    const struct tps25750_dev_config *cfg = dev->config;
    uint8_t reg_data[64];
    int ret;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    ret = i2c_burst_read_dt(
        &cfg->i2c, 
        TPS25750_REG_MODE_ADDR, 
        reg_data, 
        TPS25750_REG_MODE_SIZE);
    if (ret) {
        LOG_ERR("Failed to read MODE register: %d", ret);
        return -ENODEV;
    } else {
        // MODE register contains ASCII values
        reg_data[TPS25750_REG_MODE_SIZE] = '\0';
        LOG_INF("MODE: %s", (char*) reg_data);
    }

    return 0;
}

static int tps25750_init(const struct device *dev)
{
    struct tps25750_dev_data *data = dev->data;
    const struct tps25750_dev_config *cfg = dev->config;
    uint8_t reg_data[64];
    int ret;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    ret = i2c_burst_read_dt(
        &cfg->i2c, 
        TPS25750_REG_MODE_ADDR, 
        reg_data, 
        TPS25750_REG_MODE_SIZE);
    if (ret) {
        LOG_ERR("Failed to read MODE register: %d", ret);
        return -ENODEV;
    } else {
        // MODE register contains ASCII values
        reg_data[TPS25750_REG_MODE_SIZE] = '\0';
        LOG_INF("MODE: %s", (char*) reg_data);
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