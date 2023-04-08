#include <zephyr/init.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/tps25750/tps25750.h>
#include "tps25750_priv.h"

#include <string.h>

#define DT_DRV_COMPAT ti_tps25750

LOG_MODULE_REGISTER(tps25750, LOG_LEVEL_INF);

/**
 * @brief Construct a byte array that can be sent to one of the INT registers based on
 * a tps25750_int_t
 * 
 * @param i 
 * @param bytes 
 */
void _tps25750_int_to_bytes(const tps25750_int_t* i, uint8_t* bytes) {
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
void _bytes_to_tps25750_int(const uint8_t* bytes, tps25750_int_t* i) {
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

    uint8_t bytes[TPS25750_REG_INT_SIZE + 1];

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
    
    // Skip the first byte, which contains a useless "data size"
    _bytes_to_tps25750_int(&bytes[1], i);

    LOG_HEXDUMP_DBG(bytes, sizeof(bytes), "INT_EVENT1");

    return 0;
}

int tps25750_read_int_mask1(const struct device *dev, tps25750_int_t* i) {
    if (!dev || !i) {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    uint8_t bytes[TPS25750_REG_INT_SIZE + 1];

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
    
    _bytes_to_tps25750_int(&bytes[1], i);

    LOG_HEXDUMP_DBG(bytes, sizeof(bytes), "INT_MASK1");

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
        (uint8_t*)mode, 
        sizeof(tps25750_mode_t));
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
        (uint8_t*)cmd, 
        sizeof(tps25750_cmd1_t));
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

    // Oh TI... why do you always hurt me like this
    // Since we know this value, set it correctly here.
    cmd->byte_count = sizeof(tps25750_cmd1_t) - sizeof(cmd->byte_count);

    return i2c_burst_write_dt(
        &cfg->i2c, 
        TPS25750_REG_CMD1_ADDR, 
        (uint8_t*)cmd, 
        sizeof(tps25750_cmd1_t));
}

int tps25750_write_data1(const struct device *dev, const tps25750_data1_t* data) {
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
        TPS25750_REG_DATA1_ADDR, 
        (uint8_t*)data,
        // Oh TI... why do you always hurt me like this
        data->byte_count + sizeof(data->byte_count));
}

int tps25750_read_data1(const struct device *dev, const tps25750_data1_t* data) {
    if (!dev) {
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
        TPS25750_REG_DATA1_ADDR, 
        (uint8_t*)data,
        sizeof(tps25750_data1_t));
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

int tps25750_download_patch(const struct device *dev) {
    int ret;
    tps25750_data1_t data;
    tps25750_cmd1_t cmd;

    // Check what mode we are in
    tps25750_mode_t mode;
    ret = tps25750_read_mode(dev, &mode);
    if (ret) {
        LOG_ERR("tps25750_read_mode: %d", ret);
        return ret;
    }

    // If we are in mode == PTCH, we can proceed
    if (strncmp(mode.mode, TPS25750_REG_MODE_VAL_PTCH, sizeof(mode.mode) != 0)) {
        LOG_ERR("MODE is not PTCH! Cannot download patch");
        return -EBUSY;
    }

    // Now check that we have ReadyForPatch in INT_EVENT1
    tps25750_int_t i;
    ret = tps25750_read_int_event1(dev, &i);
    if (ret) {
        LOG_ERR("tps25750_read_int_event1: %d", ret);
        return ret;
    } 

    if (!i.ReadyForPatch) {
        LOG_ERR("INT_EVENT1 does not indicate ReadyForPatch!");
        return -EBUSY;
    }

    // We are actually ready to patch: construct a patch start command
    data.byte_count = 6;
    data.data[0] = 0; // Byte1 of bundle size
    data.data[1] = 0; // Byte2 of bundle size
    data.data[2] = 0; // Byte3 of bundle size
    data.data[3] = 0; // Byte4 of bundle size
    data.data[4] = 0x15; // Slave Address #2, 6 bit. 0x15 chosen at random
    data.data[5] = 0x32; // Timeout, 0x32 == 5 seconds

    ret = tps25750_write_data1(dev, &data);
    if (ret) {
        LOG_ERR("tps25750_write_data1: %d", ret);
        return ret;
    }

    // Send a "PBMs" command to the device to start the programming sequence
    cmd.byte_count = TPS25750_REG_CMD1_SIZE;
    strncpy(cmd.cmd, TPS25750_REG_CMD1_VAL_PBMS, sizeof(cmd.cmd));

    ret = tps25750_write_cmd1(dev, &cmd);
    if (ret) {
        LOG_ERR("tps25750_write_cmd1: %d", ret);
        return ret;
    }

    // Readback the CMD1 register to check command status
    do {
        ret = tps25750_read_cmd1(dev, &cmd);
        if (ret) {
            LOG_ERR("tps25750_read_cmd1: %d", ret);
            return ret;
        }

        // If we get TPS25750_REG_CMD1_VAL_ERROR then an error ocurred
        if (strncmp(cmd.cmd, TPS25750_REG_CMD1_VAL_ERROR, sizeof(cmd.cmd) == 0)) {
            LOG_ERR("CMD1 register indicates command error!");
            return -EBUSY;
        }

        if (cmd.cmd[0] == '\0') {
            LOG_INF("Controller accepted PBMs command + data payload!");
            break;
        }

        // PD controller isn't finished processing the command yet: sleep for a bit
        k_msleep(10);
    } while (true);

    // Read DATA1 to see what our patch status is
    ret = tps25750_read_data1(dev, &data);
    if (ret) {
        LOG_ERR("tps25750_read_data1: %d", ret);
        return ret;
    }

    if (data.data[0] != 0) {
        LOG_ERR("PD Controller returned error starting patch: %u", data.data[0]);
        return -EFAULT;
    }

    LOG_INF("Patch complete!");

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