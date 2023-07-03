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

#define BIT_EXTRACT(_val, _start, _num) (((_val) & (((1 << _num) - 1) << _start)) >> _start)

/* X-Macro Definitions */

/* Define bq25792_read_REG() functions */
#define BQ25792_BIT(_name, _bitLowest, _numBits) \
    dst->_name = BIT_EXTRACT(dst->raw, _bitLowest, _numBits);

#define REG(_regName) \
    int bq25792_read_##_regName(const struct device *dev, _regName##_t* dst) { \
        const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config*)dev->config; \
        int ret = i2c_burst_read_dt( \
            &cfg->i2c, \
            _regName##_ADDR, \
            &(dst->raw), \
            sizeof(dst->raw)); \
        if (ret) {   \
            LOG_ERR("I2C Read failed: %d", ret); \
            return ret; \
        } \
        _regName##_BITS \
        return 0; \
    }

REG_LIST
#undef BQ25792_BIT
#undef REG

/* Define bq25792_write_REG() functions */
#define BQ25792_BIT(_name, _bitLowest, _numBits) \
    dst->raw |= (dst->_name & ((1 << _numBits) - 1)) << _bitLowest;

#define REG(_regName) \
    int bq25792_write_##_regName(const struct device *dev, _regName##_t* dst) { \
        const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config*)dev->config; \
        dst->raw = 0; \
        _regName##_BITS \
        LOG_INF("Writing 0x%02x", dst->raw);\
        int ret = i2c_burst_write_dt( \
            &cfg->i2c, \
            _regName##_ADDR, \
            &(dst->raw), \
            sizeof(dst->raw)); \
        if (ret) {   \
            LOG_ERR("I2C Write failed: %d", ret); \
            return ret; \
        } \
        return 0; \
    }

REG_LIST
#undef BQ25792_BIT
#undef REG

/* Define bq25792_dump_REG() functions */
#define BQ25792_BIT(_name, _bitLowest, _numBits) \
    LOG_INF(#_name ": %u", r._name);

#define REG(_regName) \
    int bq25792_dump_##_regName(const struct device *dev) { \
        _regName##_t r; \
        int ret = bq25792_read_##_regName(dev, &r); \
        if (ret) {   \
            LOG_ERR("Reg Read failed: %d", ret); \
            return ret; \
        } \
        LOG_INF("Reading " #_regName ": %u", r.raw); \
        _regName##_BITS \
        return 0; \
    }

REG_LIST
#undef BQ25792_BIT
#undef REG

// This is the _only_ register in the entire chip that requires a 2 byte write. 
// Instead of adding complex logic to the X-macros, just define this one by hand
int bq25792_read_BQ25792_REG_ICO_CURRENT_LIMIT(const struct device *dev, uint16_t* currentLimit_mA) {
    const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config*)dev->config; 
    int ret = i2c_burst_read_dt( 
        &cfg->i2c, 
        BQ25792_REG_ICO_CURRENT_LIMIT_ADDR, 
        (uint8_t*)&(currentLimit_mA), 
        sizeof(currentLimit_mA));

    if (ret) {
        LOG_ERR("I2C Read failed: %d", ret);
        return ret; 
    } 

    return 0;
}

int bq25792_dump_BQ25792_REG_ICO_CURRENT_LIMIT(const struct device *dev) {
    uint16_t currentLimit_mA; 
    int ret = bq25792_read_BQ25792_REG_ICO_CURRENT_LIMIT(dev, &currentLimit_mA); 
    if (ret) {   
        LOG_ERR("Reg Read failed: %d", ret); 
        return ret; 
    }

    LOG_INF("Reading BQ25792_REG_ICO_CURRENT_LIMIT: %u", currentLimit_mA);
    LOG_INF("ICO_ILIM: %u mA", currentLimit_mA * BQ25792_REG_ICO_CURRENT_LIMIT_ICO_ILIM_SCALE);
    return 0;
}

int bq25792_read_BQ25792_REG_INPUT_CURRENT_LIMIT(const struct device *dev, uint16_t* currentLimit_mA) {
    const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config*)dev->config; 
    int ret = i2c_burst_read_dt( 
        &cfg->i2c, 
        BQ25792_REG_INPUT_CURRENT_LIMIT_ADDR, 
        (uint8_t*)&(currentLimit_mA), 
        sizeof(currentLimit_mA));

    if (ret) {
        LOG_ERR("I2C Read failed: %d", ret); 
        return ret; 
    } 

    return 0;
}

int bq25792_dump_BQ25792_REG_INPUT_CURRENT_LIMIT(const struct device *dev) {
    uint16_t currentLimit_mA; 
    int ret = bq25792_read_BQ25792_REG_INPUT_CURRENT_LIMIT(dev, &currentLimit_mA); 
    if (ret) {   
        LOG_ERR("Reg Read failed: %d", ret); 
        return ret; 
    }

    LOG_INF("Reading BQ25792_REG_INPUT_CURRENT_LIMIT: %u", currentLimit_mA);
    LOG_INF("IINDPM: %u mA", currentLimit_mA * BQ25792_REG_INPUT_CURRENT_LIMIT_IINDPM_SCALE);
    return 0;
}

/* X-Macro Definition for bq25792_dump() function */

#define REG(_regName) \
    ret = bq25792_dump_##_regName(dev); \
    if (ret) {   \
        LOG_ERR("Dumping register " #_regName " failed: %d", ret); \
        return ret; \
    } \
    k_msleep(200);

int bq25792_dump(const struct device *dev)
{
    if (!dev)
    {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct bq25792_dev_config *cfg = (const struct bq25792_dev_config*)dev->config;
    int ret;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    REG_LIST
    
    {
        BQ25792_CHARGE_VOLTAGE_LIMIT tmp(cfg);
        tmp.dump();
    }
    {
        BQ25792_CHARGER_STATUS_0 tmp(cfg);
        tmp.dump();
    }
    {
        BQ25792_CHARGER_STATUS_1 tmp(cfg);
        tmp.dump();
    }
    {
        BQ25792_CHARGER_STATUS_2 tmp(cfg);
        tmp.dump();
    }
    {
        BQ25792_CHARGER_STATUS_3 tmp(cfg);
        tmp.dump();
    }
    {
        BQ25792_CHARGER_STATUS_4 tmp(cfg);
        tmp.dump();
    }

    ret = bq25792_dump_BQ25792_REG_ICO_CURRENT_LIMIT(dev);
    if (ret) {
        LOG_ERR("Dumping register BQ25792_REG_ICO_CURRENT_LIMIT failed: %d", ret);
        return ret;
    }

    ret = bq25792_dump_BQ25792_REG_INPUT_CURRENT_LIMIT(dev);
    if (ret) {
        LOG_ERR("Dumping register BQ25792_REG_INPUT_CURRENT_LIMIT failed: %d", ret);
        return ret;
    }

    return 0;
}
#undef REG

int bq25792_temp_override(const struct device *dev, bool enable) {
    BQ25792_REG_NTC_CONTROL_1_t reg;
    int ret = bq25792_read_BQ25792_REG_NTC_CONTROL_1(dev, &reg);
    if (ret) {
        LOG_ERR("Failed to read NTC_CONTROL_1: %d", ret);
        return ret;
    }

    LOG_INF("Setting TS_IGNORE to %d", enable ? 1 : 0);

    reg.TS_IGNORE = enable ? 1 : 0;

    ret = bq25792_write_BQ25792_REG_NTC_CONTROL_1(dev, &reg);
    if (ret) {
        LOG_ERR("Failed to write NTC_CONTROL_1: %d", ret);
        return ret;
    }

    return 0;
}