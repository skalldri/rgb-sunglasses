#include <zephyr/init.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/bq25792/bq25792.h>
#include "bq25792_init.h"

LOG_MODULE_REGISTER(bq25792, LOG_LEVEL_INF);

// Must be included after LOG_MODULE_REGISTER() since this contains LOG_XXX statements
// TODO: fix this!
#include "bq25792_priv.h"

/* X-Macro Definition for bq25792_dump() function */
#define REG(_regName) \
    { \
        _regName tmp(cfg); \
        tmp.dump(); \
    }

int bq25792_dump(const struct device *dev)
{
    if (!dev)
    {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config*)dev->config;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    REG_LIST

    return 0;
}
#undef REG

int bq25792_temp_override(const struct device *dev, bool enable) {
    if (!dev)
    {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config*)dev->config;

    BQ25792_NTC_CONTROL_1 reg(cfg);
    
    uint32_t ts_ignore = enable ? 1 : 0;
    LOG_INF("Setting TS_IGNORE to %u", ts_ignore);

    return reg.set<BQ25792_NTC_CONTROL_1_TS_IGNORE>(ts_ignore, true /* flush */);
}