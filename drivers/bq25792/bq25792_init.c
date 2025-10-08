#include <zephyr/init.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/bq25792/bq25792.h>
#include "bq25792_init.h"

#define DT_DRV_COMPAT ti_bq25792

static int bq25792_init(const struct device *dev)
{
    return 0;
}

#define BQ25792_DEFINE(inst)                                            \
    static struct bq25792_dev_data bq25792_data_##inst;                 \
                                                                        \
    static const struct bq25792_dev_config bq25792_config_##inst =      \
        {                                                               \
            .i2c = I2C_DT_SPEC_INST_GET(inst)};                         \
                                                                        \
    DEVICE_DT_INST_DEFINE(inst, bq25792_init, NULL,                     \
                          &bq25792_data_##inst, &bq25792_config_##inst, \
                          POST_KERNEL, CONFIG_BQ25792_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(BQ25792_DEFINE)