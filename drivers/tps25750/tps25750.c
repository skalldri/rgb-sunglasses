#include <zephyr/init.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/tps25750/tps25750.h>
#include "tps25750_priv.h"

#if defined(CONFIG_TPS25750_INTERNAL_PATCH)

#include CONFIG_TPS25750_PATCH_HEADER_FILE

#endif

#if defined(CONFIG_TPS25750_COMPRESSED_PATCH)
#include "lz4.h"
#endif

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
void _tps25750_int_to_bytes(const tps25750_int_t *i, uint8_t *bytes)
{
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
void _bytes_to_tps25750_int(const uint8_t *bytes, tps25750_int_t *i)
{
#define TPS25750_INT_BIT(_name, _byte, _bit) \
    i->_name = tps25750_int_bit_##_name(bytes);

    TPS25750_INT_BIT_LIST
#undef TPS25750_INT_BIT
}

#if defined(CONFIG_TPS25750_INTERNAL_PATCH)

int tps25750_get_patch(const char **patch, size_t *patch_size)
{
    if (!patch || !patch_size)
    {
        LOG_ERR("NULL pointer");
        return -EINVAL;
    }

#if defined(CONFIG_TPS25750_COMPRESSED_PATCH)
    static char decompressed_patch[TPS25750_PATCH_UNCOMPRESSED_SIZE];
    static bool is_decompressed = false;

    if (!is_decompressed)
    {
        // Decompress using LZ4
        int decompressed_size = LZ4_decompress_safe(
            tps25750_patch,
            decompressed_patch,
            TPS25750_PATCH_COMPRESSED_SIZE,
            TPS25750_PATCH_UNCOMPRESSED_SIZE);

        if (decompressed_size < 0)
        {
            LOG_ERR("LZ4 decompression failed with error code %d", decompressed_size);
            return decompressed_size;
        }

        LOG_INF("LZ4 Decompression complete!");
        is_decompressed = true;
    }

    *patch = decompressed_patch;
    *patch_size = TPS25750_PATCH_UNCOMPRESSED_SIZE;
#else
    *patch = tps25750_patch;
    *patch_size = TPS25750_PATCH_UNCOMPRESSED_SIZE;
#endif

    return 0;
}

#endif

int tps25750_read_int_event1(const struct device *dev, tps25750_int_t *i)
{
    if (!dev || !i)
    {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    uint8_t bytes[TPS25750_REG_INT_SIZE + 1];

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    int ret = i2c_burst_read_dt(
        &cfg->i2c,
        TPS25750_REG_INT_EVENT1_ADDR,
        bytes,
        sizeof(bytes));

    if (ret)
    {
        return ret;
    }

    // Skip the first byte, which contains a useless "data size"
    _bytes_to_tps25750_int(&bytes[1], i);

    LOG_HEXDUMP_DBG(bytes, sizeof(bytes), "INT_EVENT1");

    return 0;
}

int tps25750_read_int_mask1(const struct device *dev, tps25750_int_t *i)
{
    if (!dev || !i)
    {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    uint8_t bytes[TPS25750_REG_INT_SIZE + 1];

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    int ret = i2c_burst_read_dt(
        &cfg->i2c,
        TPS25750_REG_INT_MASK1_ADDR,
        bytes,
        sizeof(bytes));

    if (ret)
    {
        return ret;
    }

    _bytes_to_tps25750_int(&bytes[1], i);

    LOG_HEXDUMP_DBG(bytes, sizeof(bytes), "INT_MASK1");

    return 0;
}

int tps25750_read_mode(const struct device *dev, tps25750_mode_t *mode)
{
    if (!dev || !mode)
    {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_read_dt(
        &cfg->i2c,
        TPS25750_REG_MODE_ADDR,
        (uint8_t *)mode,
        sizeof(*mode));
}

int tps25750_read_cmd1(const struct device *dev, tps25750_cmd1_t *cmd)
{
    if (!dev || !cmd)
    {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_read_dt(
        &cfg->i2c,
        TPS25750_REG_CMD1_ADDR,
        (uint8_t *)cmd,
        sizeof(*cmd));
}

int tps25750_read_cmd_status(const struct device *dev)
{
    tps25750_cmd1_t cmd;

    // Readback the CMD1 register to check command status
    do
    {
        int ret = tps25750_read_cmd1(dev, &cmd);
        if (ret)
        {
            LOG_ERR("tps25750_read_cmd1: %d", ret);
            return ret;
        }

        // If we get TPS25750_REG_CMD1_VAL_ERROR then an error ocurred
        if (strncmp(cmd.cmd, TPS25750_REG_CMD1_VAL_ERROR, sizeof(cmd.cmd) == 0))
        {
            LOG_ERR("CMD1 register indicates command error!");
            return -EBUSY;
        }

        if (cmd.cmd[0] == '\0')
        {
            // CMD register containing NULL indicates the command was accepted and executed!
            break;
        }

        // PD controller isn't finished processing the command yet: sleep for a bit
        k_msleep(10);
    } while (true);

    return 0;
}

int tps25750_write_cmd1(const struct device *dev, const char *command)
{
    if (!dev)
    {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    tps25750_cmd1_t cmd;

    // Oh TI... why do you always hurt me like this
    // Since we know this value, set it correctly here.
    cmd.byte_count = sizeof(tps25750_cmd1_t) - sizeof(cmd.byte_count);
    memset(cmd.cmd, 0, sizeof(cmd.cmd));
    strncpy(cmd.cmd, command, sizeof(cmd.cmd));

    return i2c_burst_write_dt(
        &cfg->i2c,
        TPS25750_REG_CMD1_ADDR,
        (uint8_t *)&cmd,
        sizeof(cmd));
}

int tps25750_write_data1(const struct device *dev, const tps25750_data1_t *data)
{
    if (!dev)
    {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_write_dt(
        &cfg->i2c,
        TPS25750_REG_DATA1_ADDR,
        (uint8_t *)data,
        // Oh TI... why do you always hurt me like this
        data->byte_count + sizeof(data->byte_count));
}

int tps25750_read_data1(const struct device *dev, tps25750_data1_t *data)
{
    if (!dev || !data)
    {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_read_dt(
        &cfg->i2c,
        TPS25750_REG_DATA1_ADDR,
        (uint8_t *)data,
        sizeof(*data));
}

int tps25750_read_device_info(const struct device *dev, tps25750_device_info_t *info)
{
    if (!dev || !info)
    {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_read_dt(
        &cfg->i2c,
        TPS25750_REG_DEVICE_INFO_ADDR,
        (uint8_t *)info,
        sizeof(*info));
}

int tps25750_read_boot_status(const struct device *dev, tps25750_boot_status_t *status)
{
    if (!dev || !status)
    {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_read_dt(
        &cfg->i2c,
        TPS25750_REG_BOOT_STATUS_ADDR,
        (uint8_t *)status,
        sizeof(*status));
}

int tps25750_dump(const struct device *dev)
{
    if (!dev)
    {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

#if defined(CONFIG_DUMP_DEVICE_REGISTERS)

    tps25750_mode_t mode;
    int ret = tps25750_read_mode(dev, &mode);
    if (ret)
    {
        LOG_ERR("tps25750_read_mode: %d", ret);
        return ret;
    }
    LOG_INF("MODE: %.*s", sizeof(mode.mode), mode.mode);

    tps25750_int_t i;
    ret = tps25750_read_int_event1(dev, &i);
    if (ret)
    {
        LOG_ERR("tps25750_read_int_event1: %d", ret);
        return ret;
    }

// Use X-macro magic to make dumping the gigantic INT_EVENT1 register less painful
#define TPS25750_INT_BIT(_name, _byte, _bit) \
    LOG_INF("EVENT %s: %d", #_name, i._name);

    TPS25750_INT_BIT_LIST
#undef TPS25750_INT_BIT

    /*
        ret = tps25750_read_int_mask1(dev, &i);
        if (ret)
        {
            LOG_ERR("tps25750_read_int_mask1: %d", ret);
            return ret;
        }

    // Use X-macro magic to make dumping the gigantic INT_MASK1 register less painful
    #define TPS25750_INT_BIT(_name, _byte, _bit) \
        LOG_INF("MASK %s: %d", #_name, i._name);

        TPS25750_INT_BIT_LIST
    #undef TPS25750_INT_BIT
    */

    tps25750_device_info_t info;
    ret = tps25750_read_device_info(dev, &info);
    if (ret)
    {
        LOG_ERR("tps25750_read_device_info: %d", ret);
        return ret;
    }
    LOG_INF("Device Info: %.*s", sizeof(info.str), info.str);

    tps25750_boot_status_t status;
    ret = tps25750_read_boot_status(dev, &status);
    if (ret)
    {
        LOG_ERR("tps25750_read_boot_status: %d", ret);
        return ret;
    }
    LOG_INF("Boot Status:");
    LOG_INF("PatchHeaderErr: %u", (status.boot_flags[0] & 0b1) ? 1 : 0);
    LOG_INF("DeadBatteryFlag: %u", (status.boot_flags[0] & 0b100) ? 1 : 0);
    LOG_INF("I2cEepromPresent: %u", (status.boot_flags[0] & 0b1000) ? 1 : 0);
    LOG_INF("patchdownloaderr: %u", (status.boot_flags[1] & 0b100) ? 1 : 0);
    LOG_INF("patchdownloaderr: %u", (status.boot_flags[2] & 0b1000) ? 1 : 0);
    LOG_INF("PatchConfigSource: %u", (status.boot_flags[3] >> 5));
    LOG_INF("PD Controller Revision: %u", status.revision_id);

    LOG_INF("Dump complete!");

#endif

    return 0;
}

int tps25750_download_patch(const struct device *dev, const char *patch, uint32_t patchSize)
{
    if (!dev)
    {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    int ret;
    tps25750_data1_t data;
    const struct tps25750_dev_config *cfg = dev->config;

    // Check what mode we are in
    tps25750_mode_t mode;
    ret = tps25750_read_mode(dev, &mode);
    if (ret)
    {
        LOG_ERR("tps25750_read_mode: %d", ret);
        return ret;
    }

    // If we are in mode == PTCH, we can proceed
    if (strncmp(mode.mode, TPS25750_REG_MODE_VAL_PTCH, sizeof(mode.mode) != 0))
    {
        // Check if we are in APP mode
        if (strncmp(mode.mode, TPS25750_REG_MODE_VAL_APP, sizeof(mode.mode) == 0))
        {
            LOG_INF("Patch already loaded!");
            return 0;
        }
        else
        {
            LOG_ERR("MODE is not PTCH (got %.*s) Cannot download patch!", sizeof(mode.mode), mode.mode);
            return -EBUSY;
        }
    }

    // Now check that we have ReadyForPatch in INT_EVENT1
    tps25750_int_t i;
    ret = tps25750_read_int_event1(dev, &i);
    if (ret)
    {
        LOG_ERR("tps25750_read_int_event1: %d", ret);
        return ret;
    }

    if (!i.ReadyForPatch)
    {
        LOG_ERR("INT_EVENT1 does not indicate ReadyForPatch!");
        return -EBUSY;
    }

    if (patch)
    {
        // We are actually ready to patch: construct a patch start command
        data.byte_count = 6;
        data.data[0] = (uint8_t)((patchSize >> 0) & 0xFF);  // Byte1 of bundle size
        data.data[1] = (uint8_t)((patchSize >> 8) & 0xFF);  // Byte2 of bundle size
        data.data[2] = (uint8_t)((patchSize >> 16) & 0xFF); // Byte3 of bundle size
        data.data[3] = (uint8_t)((patchSize >> 24) & 0xFF); // Byte4 of bundle size
        data.data[4] = cfg->patch_address;                  // Slave Address #2, 6 bit. 0x15 chosen at random
        data.data[5] = 0x32;                                // Timeout, 0x32 == 5 seconds

        ret = tps25750_write_data1(dev, &data);
        if (ret)
        {
            LOG_ERR("tps25750_write_data1: %d", ret);
            return ret;
        }

        // Send a "PBMs" command to the device to start the programming sequence
        ret = tps25750_write_cmd1(dev, TPS25750_REG_CMD1_VAL_PBMS);
        if (ret)
        {
            LOG_ERR("tps25750_write_cmd1: %d", ret);
            return ret;
        }

        // Readback the CMD1 register to check command status
        ret = tps25750_read_cmd_status(dev);
        if (ret)
        {
            LOG_ERR("tps25750_read_cmd_status: %d", ret);
            return ret;
        }
        else
        {
            LOG_INF("Controller accepted PBMs command + data payload!");
        }

        // Read DATA1 to see what our patch status is
        ret = tps25750_read_data1(dev, &data);
        if (ret)
        {
            LOG_ERR("tps25750_read_data1: %d", ret);
            return ret;
        }

        if (data.data[0] != 0)
        {
            LOG_ERR("PD Controller returned error starting patch: %u", data.data[0]);
            return -EFAULT;
        }

        // OK! We are ready to patch at this point.

        // Construct a new I2C spec that uses the patch address
        struct i2c_dt_spec i2c_patch = cfg->i2c;
        i2c_patch.addr = cfg->patch_address;

        uint32_t remainingBytes = patchSize;
        const char *writePointer = patch;

        LOG_INF("Starting download of ROM patch");
        LOG_INF("Patch size %u bytes", patchSize);

        do
        {
            uint32_t writeSize = MIN(cfg->patch_chunk_size, remainingBytes);
            memcpy(cfg->patch_buffer, writePointer, writeSize);

            ret = i2c_write_dt(&i2c_patch,
                               cfg->patch_buffer,
                               writeSize);
            if (ret)
            {
                LOG_ERR("Failed writing patch: %d", ret);
                return ret;
            }

            writePointer += writeSize;
            remainingBytes -= writeSize;
        } while (remainingBytes > 0);

        // TI documentation indicates we must wait at least 500us between sending the last
        // patch byte and writing the PBMc command
        // k_usleep(500);
        k_msleep(10);

        LOG_INF("Patch data sent. Sending PBMc command...");
    }
    else
    {
        LOG_INF("No patch data. Sending PBMc command...");
    }

    // Send a "PBMc" command to indicate that all patch data has been loaded
    ret = tps25750_write_cmd1(dev, TPS25750_REG_CMD1_VAL_PBMC);
    if (ret)
    {
        LOG_ERR("tps25750_write_cmd1: %d", ret);
        return ret;
    }

    uint8_t retries = 0;
    do
    {
        // Wait 50ms... the controller is busy CRC-ing the patch and may NAK
        // Ideally we want to not experience a NAK if we can avoid it
        k_msleep(50);

        // Readback the CMD1 register to check command status
        ret = tps25750_read_cmd_status(dev);
        if (ret)
        {
            LOG_ERR("PBMC: tps25750_read_cmd_status: %d", ret);
        }
        else
        {
            LOG_INF("Controller accepted PBMc command!");
            break;
        }
    } while (ret && (retries++ < 10));

    if (ret)
    {
        LOG_ERR("Failed reading PBMC command status after %d retries", retries);
        return ret;
    }

    // TI docs say to wait at least 20ms before attempting to read DATA1 register after
    // a patch download
    k_msleep(20);

    // Read the contents of the DATA1 register
    ret = tps25750_read_data1(dev, &data);
    if (ret)
    {
        LOG_ERR("tps25750_read_data1: %d", ret);
        return ret;
    }

    // Check byte1 of the returned data for status
    if (data.data[0] != 0)
    {
        LOG_ERR("Patch status was not success! %d", data.data[0]);
        LOG_HEXDUMP_ERR(data.data, sizeof(data.data), "PBMc Return");
        return -EFAULT;
    }

    LOG_INF("Patch download complete! Checking mode...");

    // Check that MODE is now "APP "
    ret = tps25750_read_mode(dev, &mode);
    if (ret)
    {
        LOG_ERR("tps25750_read_mode: %d", ret);
        return ret;
    }

    // If we are in mode == APP, patch-loading is complete!
    if (strncmp(mode.mode, TPS25750_REG_MODE_VAL_APP, sizeof(mode.mode) != 0))
    {
        LOG_ERR("MODE is not APP! Patch download failed!");
        return -EFAULT;
    }

    LOG_INF("Patch complete!");

    return 0;
}

int tps25750_clear_dead_battery(const struct device *dev)
{
    if (!dev)
    {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;
    int ret;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    // Send a "DBfg" command to the device to clear the dead battery flag
    ret = tps25750_write_cmd1(dev, TPS25750_REG_CMD1_VAL_DBFG);
    if (ret)
    {
        LOG_ERR("tps25750_write_cmd1: %d", ret);
        return ret;
    }

    // Readback the CMD1 register to check command status
    ret = tps25750_read_cmd_status(dev);
    if (ret)
    {
        LOG_ERR("'%s': tps25750_read_cmd_status: %d", TPS25750_REG_CMD1_VAL_DBFG, ret);
        return ret;
    }

    LOG_INF("Controller accepted '%s' command!", TPS25750_REG_CMD1_VAL_DBFG);

    return 0;
}

#define TPS25750_WORKQ_STACK_SIZE 1024
#define TPS25750_WORKQ_PRIORITY 5

K_THREAD_STACK_DEFINE(tps25750_workq_stack_area, TPS25750_WORKQ_STACK_SIZE);
struct k_work_q tps25750_work_q;

void tps25750_irq_work(struct k_work *item)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(item);
    struct tps25750_dev_data *data = CONTAINER_OF(dwork, struct tps25750_dev_data, work);
    const struct device *dev = data->dev;
    const struct tps25750_dev_config *cfg = dev->config;
    int ret = 0;

    LOG_INF("dev: %p", dev);
    LOG_INF("cfg: %p", cfg);
    LOG_INF("data: %p", data);

    // Figure out what caused the interrupt
    tps25750_int_t interrupt;
    ret = tps25750_read_int_event1(dev, &interrupt);
    if (ret)
    {
        LOG_ERR("tps25750_read_int_event1: %d", ret);
        return;
    }

    // TODO: need to refactor this to handle other kinds of interrupts
    if (interrupt.ReadyForPatch)
    {
        LOG_INF("TPS25750 IRQ: ReadyForPatch!");

#if defined(CONFIG_TPS25750_INTERNAL_PATCH)
        // Read the current mode
        tps25750_mode_t mode;
        int ret = tps25750_read_mode(data->dev, &mode);
        if (ret)
        {
            LOG_ERR("tps25750_read_mode: %d", ret);
            return;
        }
        LOG_INF("MODE: %.*s", sizeof(mode.mode), mode.mode);

        // If we are in mode == PTCH, we can proceed
        if (strncmp(mode.mode, TPS25750_REG_MODE_VAL_PTCH, sizeof(mode.mode) != 0))
        {
            // Check if we are in APP mode
            if (strncmp(mode.mode, TPS25750_REG_MODE_VAL_APP, sizeof(mode.mode) == 0))
            {
                LOG_INF("Patch already loaded!");
                return;
            }
            else
            {
                LOG_ERR("MODE is not PTCH (got %.*s) Cannot download patch!", sizeof(mode.mode), mode.mode);
                return;
            }
        }

        const char *patch;
        size_t size;
        ret = tps25750_get_patch(&patch, &size);
        if (ret)
        {
            LOG_ERR("tps25750_get_patch: %d", ret);
            return;
        }

        ret = tps25750_download_patch(data->dev, patch, size);
        if (ret)
        {
            LOG_ERR("Patch download failed! %d", ret);
        }
        else
        {
            LOG_INF("Patch download success!");
        }
#endif // CONFIG_TPS25750_INTERNAL_PATCH
    }
}

void tps25750_irq(const struct device *dev, const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
    struct tps25750_dev_data *data = (struct tps25750_dev_data *)dev->data;

    k_work_schedule_for_queue(&tps25750_work_q, &data->work, K_MSEC(3000));

    LOG_INF("Got a TPS25750 callback!");
}

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int tps25750_init(const struct device *dev)
{
    const struct tps25750_dev_config *cfg = dev->config;
    struct tps25750_dev_data *data = (struct tps25750_dev_data *)dev->data;
    int ret = 0;
    data->dev = dev;

    LOG_INF("dev: %p", dev);
    LOG_INF("cfg: %p", cfg);
    LOG_INF("data: %p", data);

    k_work_queue_init(&tps25750_work_q);
    k_work_queue_start(&tps25750_work_q, tps25750_workq_stack_area,
                       K_THREAD_STACK_SIZEOF(tps25750_workq_stack_area), TPS25750_WORKQ_PRIORITY,
                       NULL);

    k_work_init_delayable(&data->work, tps25750_irq_work);

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

#if defined(CONFIG_TPS25750_COMPRESSED_PATCH_PRELOAD) || defined(CONFIG_TPS25750_INTERNAL_PATCH)
    const char *patch;
    size_t size;
    ret = tps25750_get_patch(&patch, &size);
    if (ret)
    {
        LOG_ERR("tps25750_get_patch: %d", ret);
        return ret;
    }
#endif

#if defined(CONFIG_TPS25750_INTERNAL_PATCH)
    LOG_INF("Patch address: 0x%X", cfg->patch_address);

    ret = tps25750_download_patch(dev, patch, size);
    if (ret)
    {
        LOG_ERR("Patch download failed! %d", ret);
    }
    else
    {
        LOG_INF("Patch download success!");
    }
#endif // CONFIG_TPS25750_INTERNAL_PATCH

    if (cfg->int_gpio.port)
    {
        gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(&cfg->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&data->callback, cfg->irq_callback, BIT(cfg->int_gpio.pin));
        gpio_add_callback(cfg->int_gpio.port, &data->callback);

        LOG_INF("TPS25750 interrupt pin configured! Port %s, pin %d", cfg->int_gpio.port->name, cfg->int_gpio.pin);
    }

    return 0;
}

static int tps25750_i2cm_write_reg(const struct device *dev, uint8_t addr, uint8_t reg, uint8_t *dataBuff, uint8_t dataSize)
{
    if (dataSize > TPS25750_MAX_I2C_WRITE)
    {
        LOG_ERR("Cannot write %u bytes: max write length is %u", dataSize, TPS25750_MAX_I2C_WRITE);
        return -ENOMEM;
    }

    tps25750_data1_t data;
    memset(&data, 0, sizeof(data));

    // Clamp dataSize
    dataSize = MIN(dataSize, TPS25750_MAX_I2C_WRITE);

    data.byte_count = 4 + dataSize;
    data.data[0] = addr;
    // TI's amazing datasheets at work: the payloadSize must also include the length of the register address
    // Even though this is a register-write command.
    // And there's no way to ommit the payload address.
    // And that isn't mentioned anywhere in the docs, just this forum thread: https://e2e.ti.com/support/power-management-group/power-management/f/power-management-forum/1097140/bq25792-i2cw-task-can-not-work/4065201?focus=true
    data.data[1] = dataSize + 1;
    // Byte 2: reserved
    data.data[3] = reg;

    if (dataBuff && dataSize > 0)
    {
        memcpy(&data.data[4], dataBuff, dataSize);
    }

    LOG_HEXDUMP_DBG(&data, data.byte_count + sizeof(data.byte_count), "Writing payload: ");

    int ret = tps25750_write_data1(dev, &data);
    if (ret)
    {
        LOG_ERR("tps25750_write_data1: %d", ret);
        return ret;
    }

    // Send an "I2Cw" command to the device to send the data out over I2C
    ret = tps25750_write_cmd1(dev, TPS25750_REG_CMD1_VAL_I2CW);
    if (ret)
    {
        LOG_ERR("tps25750_write_cmd1: %d", ret);
        return ret;
    }

    // Readback the CMD1 register to check command status
    ret = tps25750_read_cmd_status(dev);
    if (ret)
    {
        LOG_ERR("tps25750_read_cmd_status: %d", ret);
        return ret;
    }
    else
    {
        LOG_DBG("Controller accepted I2Cw command!");
    }

    // Read DATA1 to see transaction status
    ret = tps25750_read_data1(dev, &data);
    if (ret)
    {
        LOG_ERR("tps25750_read_data1: %d", ret);
        return ret;
    }

    if (data.data[0] != 0)
    {
        LOG_ERR("PD Controller I2CM write failure: %u", data.data[0]);
        return -EFAULT;
    }

    return 0;
}

static int tps25750_i2cm_read_reg(const struct device *dev, uint8_t addr, uint8_t reg, uint8_t *dataBuff, uint8_t dataSize)
{
    if (dataSize > TPS25750_MAX_I2C_READ)
    {
        LOG_ERR("Cannot read %u bytes: max read length is %u", dataSize, TPS25750_MAX_I2C_READ);
        return -ENOMEM;
    }

    tps25750_data1_t data;
    data.byte_count = 3;
    data.data[0] = addr;
    data.data[1] = reg;
    data.data[2] = MIN(dataSize, TPS25750_MAX_I2C_READ);

    LOG_HEXDUMP_DBG(&data, data.byte_count + sizeof(data.byte_count), "Writing payload: ");

    int ret = tps25750_write_data1(dev, &data);
    if (ret)
    {
        LOG_ERR("tps25750_write_data1: %d", ret);
        return ret;
    }

    // Send an "I2Cr" command to the device to send the data out over I2C
    ret = tps25750_write_cmd1(dev, TPS25750_REG_CMD1_VAL_I2CR);
    if (ret)
    {
        LOG_ERR("tps25750_write_cmd1: %d", ret);
        return ret;
    }

    // Readback the CMD1 register to check command status
    ret = tps25750_read_cmd_status(dev);
    if (ret)
    {
        LOG_ERR("tps25750_read_cmd_status: %d", ret);
        return ret;
    }
    else
    {
        LOG_DBG("Controller accepted " TPS25750_REG_CMD1_VAL_I2CR " command!");
    }

    // Read DATA1 to see transaction status
    ret = tps25750_read_data1(dev, &data);
    if (ret)
    {
        LOG_ERR("tps25750_read_data1: %d", ret);
        return ret;
    }

    if (data.data[0] != 0)
    {
        LOG_ERR("PD Controller I2CM read failure: %u", data.data[0]);
        return -EFAULT;
    }

    size_t copySize = dataSize;
    copySize = MIN(copySize, TPS25750_MAX_I2C_READ);
    copySize = MIN(copySize, data.byte_count);

    LOG_DBG("Copying %u bytes returned from peripheral", copySize);
    LOG_HEXDUMP_DBG(&data.data[1], copySize, "Read result: ");

    memcpy(dataBuff, &data.data[1], copySize);

    return 0;
}

static int i2c_tps25750_i2cm_transfer(const struct device *dev,
                                      struct i2c_msg *msgs,
                                      uint8_t num_msgs, uint16_t addr)
{
    LOG_DBG("Got I2C master command for address %u, num msgs %u", addr, num_msgs);

    if (num_msgs != 2)
    {
        LOG_ERR("Unsupported num messages %u", num_msgs);
        return -ENOTSUP;
    }

    // TPS25750 commands don't support 10-bit addressing
    if (msgs[0].flags & I2C_MSG_ADDR_10_BITS || msgs[1].flags & I2C_MSG_ADDR_10_BITS)
    {
        LOG_ERR("10 bit addressing is not supported");
        return -ENOTSUP;
    }

    // All I2C register transactions start with a write, which we expect to be a single byte.
    // We expect no other flags on the first message
    if (msgs[0].flags != I2C_MSG_WRITE || msgs[0].len != 1)
    {
        LOG_ERR("Unsupported first transaction: flags %u, len %u", msgs[0].flags, msgs[0].len);
        return -ENOTSUP;
    }

    uint8_t reg_addr = msgs[0].buf[0];

    // Now: is it a register read or a register write?
    if (msgs[1].flags & I2C_MSG_READ)
    {
        // Register read.
        // We should have the I2C_MSG_RESTART flag here, as well as I2C_MSG_STOP
        if ((msgs[1].flags & I2C_MSG_RESTART) != I2C_MSG_RESTART || (msgs[1].flags & I2C_MSG_STOP) != I2C_MSG_STOP)
        {
            LOG_ERR("Second I2C message did not have RESTART or STOP flag!");
            return -ENOTSUP;
        }

        return tps25750_i2cm_read_reg(dev, addr, reg_addr, msgs[1].buf, msgs[1].len);
    }

    // Register Write.
    // We should have the I2C_MSG_STOP flag here
    if ((msgs[1].flags & I2C_MSG_STOP) != I2C_MSG_STOP)
    {
        LOG_ERR("Second I2C message did not have STOP flag!");
        return -ENOTSUP;
    }

    return tps25750_i2cm_write_reg(dev, addr, reg_addr, msgs[1].buf, msgs[1].len);
}
#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define TPS25750_DEFINE(inst)                                                                            \
    static const struct i2c_driver_api i2c_tps25750_i2cm_driver_api_##inst = {                           \
        .transfer = i2c_tps25750_i2cm_transfer,                                                          \
    };                                                                                                   \
    void tps25750_irq_##inst(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins) \
    {                                                                                                    \
        const struct device *dev = DEVICE_DT_GET(DT_DRV_INST(inst));                                     \
        LOG_INF("Got a callback, device %p", dev);                                                       \
        tps25750_irq(dev, port, cb, pins);                                                               \
    }                                                                                                    \
                                                                                                         \
    static struct tps25750_dev_data tps25750_data_##inst;                                                \
                                                                                                         \
    static uint8_t tps25750_patch_buffer_##inst[DT_INST_PROP(inst, patch_chunk_size)];                   \
                                                                                                         \
    static const struct tps25750_dev_config tps25750_config_##inst =                                     \
        {                                                                                                \
            .i2c = I2C_DT_SPEC_INST_GET(inst),                                                           \
            .int_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, irq_gpios, {0}),                                  \
            .patch_buffer = tps25750_patch_buffer_##inst,                                                \
            .patch_address = DT_INST_PROP(inst, patch_address),                                          \
            .patch_chunk_size = DT_INST_PROP(inst, patch_chunk_size),                                    \
            .irq_callback = tps25750_irq_##inst};                                                        \
                                                                                                         \
    I2C_DEVICE_DT_INST_DEFINE(inst, tps25750_init, NULL,                                                 \
                              &tps25750_data_##inst, &tps25750_config_##inst,                            \
                              POST_KERNEL, CONFIG_TPS25750_INIT_PRIORITY,                                \
                              &i2c_tps25750_i2cm_driver_api_##inst);

DT_INST_FOREACH_STATUS_OKAY(TPS25750_DEFINE)