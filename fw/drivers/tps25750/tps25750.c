#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/tps25750/tps25750.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "tps25750_priv.h"

#if defined(CONFIG_TPS25750_INTERNAL_PATCH)

#include CONFIG_TPS25750_PATCH_HEADER_FILE

#endif

#if defined(CONFIG_TPS25750_COMPRESSED_PATCH)
#include "lz4.h"
#endif

#include <string.h>
#include <zephyr/sys/byteorder.h>

#define DT_DRV_COMPAT ti_tps25750

LOG_MODULE_REGISTER(tps25750, LOG_LEVEL_INF);

/**
 * @brief Construct a byte array that can be sent to one of the INT registers based on
 * a tps25750_int_t
 *
 * @param i
 * @param bytes
 */
void _tps25750_int_to_bytes(const tps25750_int_t *i, uint8_t *bytes) {
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
void _bytes_to_tps25750_int(const uint8_t *bytes, tps25750_int_t *i) {
#define TPS25750_INT_BIT(_name, _byte, _bit) i->_name = tps25750_int_bit_##_name(bytes);

    TPS25750_INT_BIT_LIST
#undef TPS25750_INT_BIT
}

#if defined(CONFIG_TPS25750_INTERNAL_PATCH)

int tps25750_get_patch(const char **patch, size_t *patch_size) {
    if (!patch || !patch_size) {
        LOG_ERR("NULL pointer");
        return -EINVAL;
    }

#if defined(CONFIG_TPS25750_COMPRESSED_PATCH)
    static char decompressed_patch[TPS25750_PATCH_UNCOMPRESSED_SIZE];
    static bool is_decompressed = false;

    if (!is_decompressed) {
        // Decompress using LZ4
        int decompressed_size =
            LZ4_decompress_safe(tps25750_patch, decompressed_patch, TPS25750_PATCH_COMPRESSED_SIZE,
                                TPS25750_PATCH_UNCOMPRESSED_SIZE);

        if (decompressed_size < 0) {
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

int tps25750_read_int_event1(const struct device *dev, tps25750_int_t *i) {
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

    int ret = i2c_burst_read_dt(&cfg->i2c, TPS25750_REG_INT_EVENT1_ADDR, bytes, sizeof(bytes));

    if (ret) {
        return ret;
    }

    // Skip the first byte, which contains a useless "data size"
    _bytes_to_tps25750_int(&bytes[1], i);

    LOG_HEXDUMP_DBG(bytes, sizeof(bytes), "INT_EVENT1");

    return 0;
}

int tps25750_read_int_mask1(const struct device *dev, tps25750_int_t *i) {
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

    int ret = i2c_burst_read_dt(&cfg->i2c, TPS25750_REG_INT_MASK1_ADDR, bytes, sizeof(bytes));

    if (ret) {
        return ret;
    }

    _bytes_to_tps25750_int(&bytes[1], i);

    LOG_HEXDUMP_DBG(bytes, sizeof(bytes), "INT_MASK1");

    return 0;
}

int tps25750_read_mode(const struct device *dev, tps25750_mode_t *mode) {
    if (!dev || !mode) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_read_dt(&cfg->i2c, TPS25750_REG_MODE_ADDR, (uint8_t *)mode, sizeof(*mode));
}

int tps25750_read_cmd1(const struct device *dev, tps25750_cmd1_t *cmd) {
    if (!dev || !cmd) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_read_dt(&cfg->i2c, TPS25750_REG_CMD1_ADDR, (uint8_t *)cmd, sizeof(*cmd));
}

int tps25750_read_cmd_status(const struct device *dev) {
    tps25750_cmd1_t cmd;

    // Readback the CMD1 register to check command status.
    // Bounded (~1s, vs ~10ms observed completion): a command that never completes must
    // not hang the calling thread forever -- callers hold the task mutex while polling,
    // so an unbounded loop here would wedge every other CMD1/DATA1 user too.
    for (int attempt = 0; attempt < 100; attempt++) {
        int ret = tps25750_read_cmd1(dev, &cmd);
        if (ret) {
            LOG_ERR("tps25750_read_cmd1: %d", ret);
            return ret;
        }

        // If we get TPS25750_REG_CMD1_VAL_ERROR then an error ocurred
        // (fixed misplaced paren: the comparison used to sit inside strncmp's length
        // argument, making n == 0 and this check dead code)
        if (strncmp(cmd.cmd, TPS25750_REG_CMD1_VAL_ERROR, sizeof(cmd.cmd)) == 0) {
            LOG_ERR("CMD1 register indicates command error!");
            return -EBUSY;
        }

        if (cmd.cmd[0] == '\0') {
            // CMD register containing NULL indicates the command was accepted and executed!
            return 0;
        }

        // PD controller isn't finished processing the command yet: sleep for a bit
        k_msleep(10);
    }

    LOG_ERR("Timed out waiting for CMD1 command to complete");
    return -ETIMEDOUT;
}

int tps25750_write_cmd1(const struct device *dev, const char *command) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    tps25750_cmd1_t cmd;

    // Oh TI... why do you always hurt me like this
    // Since we know this value, set it correctly here.
    cmd.byte_count = sizeof(tps25750_cmd1_t) - sizeof(cmd.byte_count);
    memset(cmd.cmd, 0, sizeof(cmd.cmd));
    strncpy(cmd.cmd, command, sizeof(cmd.cmd));

    return i2c_burst_write_dt(&cfg->i2c, TPS25750_REG_CMD1_ADDR, (uint8_t *)&cmd, sizeof(cmd));
}

int tps25750_write_data1(const struct device *dev, const tps25750_data1_t *data) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_write_dt(&cfg->i2c, TPS25750_REG_DATA1_ADDR, (uint8_t *)data,
                              // Oh TI... why do you always hurt me like this
                              data->byte_count + sizeof(data->byte_count));
}

int tps25750_read_data1(const struct device *dev, tps25750_data1_t *data) {
    if (!dev || !data) {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_read_dt(&cfg->i2c, TPS25750_REG_DATA1_ADDR, (uint8_t *)data, sizeof(*data));
}

int tps25750_read_device_info(const struct device *dev, tps25750_device_info_t *info) {
    if (!dev || !info) {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_read_dt(&cfg->i2c, TPS25750_REG_DEVICE_INFO_ADDR, (uint8_t *)info,
                             sizeof(*info));
}

int tps25750_read_boot_status(const struct device *dev, tps25750_boot_status_t *status) {
    if (!dev || !status) {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    return i2c_burst_read_dt(&cfg->i2c, TPS25750_REG_BOOT_STATUS_ADDR, (uint8_t *)status,
                             sizeof(*status));
}

/**
 * @brief Read a host-interface register's payload, stripping the leading
 * byte-count byte.
 *
 * Every unique-address register read returns [byte_count][payload...] (host
 * interface TRM SLVUC05A — same framing the tps25750_mode_t/boot_status
 * structs model with their byte_count field). If the device reports fewer
 * valid bytes than requested, the remainder of @p payload is zero-filled.
 */
static int tps25750_read_reg_payload(const struct device *dev, uint8_t reg, uint8_t len,
                                     uint8_t *payload) {
    if (!dev || !payload) {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    /* Largest payload read through here is TX_SINK_CAPS (29 bytes). */
    uint8_t buf[1 + TPS25750_REG_TX_SINK_CAPS_SIZE];
    if (len > sizeof(buf) - 1) {
        return -EINVAL;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    int ret = i2c_burst_read_dt(&cfg->i2c, reg, buf, 1 + len);
    if (ret) {
        return ret;
    }

    uint8_t valid = MIN(buf[0], len);
    memcpy(payload, buf + 1, valid);
    if (valid < len) {
        memset(payload + valid, 0, len - valid);
    }

    return 0;
}

int tps25750_read_power_status(const struct device *dev, uint16_t *raw) {
    if (!raw) {
        return -EINVAL;
    }
    /* POWER_STATUS 0x3F, 2 bytes LE (TRM Table 2-32/2-33) */
    uint8_t payload[TPS25750_REG_POWER_STATUS_SIZE];
    int ret =
        tps25750_read_reg_payload(dev, TPS25750_REG_POWER_STATUS_ADDR, sizeof(payload), payload);
    if (ret) {
        return ret;
    }
    *raw = sys_get_le16(payload);
    return 0;
}

int tps25750_read_pd_status(const struct device *dev, uint32_t *raw) {
    if (!raw) {
        return -EINVAL;
    }
    /* PD_STATUS 0x40, 4 bytes LE (TRM Table 2-34/2-35) */
    uint8_t payload[TPS25750_REG_PD_STATUS_SIZE];
    int ret =
        tps25750_read_reg_payload(dev, TPS25750_REG_PD_STATUS_ADDR, sizeof(payload), payload);
    if (ret) {
        return ret;
    }
    *raw = sys_get_le32(payload);
    return 0;
}

int tps25750_read_active_contract_pdo(const struct device *dev, uint32_t *pdo) {
    if (!pdo) {
        return -EINVAL;
    }
    /* ACTIVE_CONTRACT_PDO 0x34, 6 bytes: bytes 1-4 = active PDO as LE32,
     * bytes 5-6 = source properties (TRM Table 2-28/2-29). Cleared on
     * disconnect / Hard Reset / PR_Swap. */
    uint8_t payload[TPS25750_REG_ACTIVE_CONTRACT_PDO_SIZE];
    int ret = tps25750_read_reg_payload(dev, TPS25750_REG_ACTIVE_CONTRACT_PDO_ADDR,
                                        sizeof(payload), payload);
    if (ret) {
        return ret;
    }
    *pdo = sys_get_le32(payload);
    return 0;
}

int tps25750_read_active_contract_rdo(const struct device *dev, uint32_t *rdo) {
    if (!rdo) {
        return -EINVAL;
    }
    /* ACTIVE_CONTRACT_RDO 0x35, 4 bytes LE (TRM Table 2-30/2-31) */
    uint8_t payload[TPS25750_REG_ACTIVE_CONTRACT_RDO_SIZE];
    int ret = tps25750_read_reg_payload(dev, TPS25750_REG_ACTIVE_CONTRACT_RDO_ADDR,
                                        sizeof(payload), payload);
    if (ret) {
        return ret;
    }
    *rdo = sys_get_le32(payload);
    return 0;
}

int tps25750_read_tx_sink_caps(const struct device *dev, uint32_t *pdos, size_t max_pdos,
                               uint8_t *num_pdos) {
    if (!pdos || !num_pdos) {
        return -EINVAL;
    }
    /* TX_SINK_CAPS 0x33, 29 bytes: byte 1 = header (bits 2:0 numValidPDOs),
     * bytes 2-5 = PDO#1 LE32, ... up to PDO#7 (TRM Table 2-24/2-25). */
    uint8_t payload[TPS25750_REG_TX_SINK_CAPS_SIZE];
    int ret =
        tps25750_read_reg_payload(dev, TPS25750_REG_TX_SINK_CAPS_ADDR, sizeof(payload), payload);
    if (ret) {
        return ret;
    }

    *num_pdos = payload[0] & 0x7;
    size_t n = MIN((size_t)*num_pdos, max_pdos);
    n = MIN(n, (size_t)7);
    for (size_t i = 0; i < n; i++) {
        pdos[i] = sys_get_le32(&payload[1 + 4 * i]);
    }
    return 0;
}

int tps25750_get_pd_power_info(const struct device *dev, struct tps25750_pd_power_info *info) {
    if (!info) {
        return -EINVAL;
    }

    uint16_t ps;
    int ret = tps25750_read_power_status(dev, &ps);
    if (ret) {
        return ret;
    }

    info->raw_power_status = ps;
    info->raw_pdo = 0;
    info->raw_rdo = 0;
    /* POWER_STATUS bit 0 = PowerConnection, bit 1 = SourceSink (1b = the
     * connection provides power, i.e. we sink) — TRM Table 2-33. */
    info->connected = (ps & BIT(0)) != 0;
    info->sinking = (ps & BIT(1)) != 0;

    if (!info->connected || !info->sinking) {
        /* No connection, or WE are the source (SourceSink=0b, e.g. powering
         * an OTG peripheral): there is no input power budget to report. In
         * source mode TypeCCurrent reflects our OWN Rp advertisement (TRM
         * Table 2-33 — "If the port is connected as source, the field is
         * updated upon initial connection only"), not anything a partner
         * offers — decoding it here would fabricate a phantom input budget. */
        info->source = TPS25750_PWR_NONE;
        info->available_mv = 0;
        info->available_ma = 0;
        return 0;
    }

    /* TypeCCurrent bits 3:2 — 00b USB default / 01b 1.5A / 10b 3.0A /
     * 11b explicit PD contract (TRM Table 2-33). */
    switch ((ps >> 2) & 0x3) {
        case 0x0:
            info->source = TPS25750_PWR_TYPEC_DEFAULT;
            info->available_mv = 5000;
            info->available_ma = 500;
            break;
        case 0x1:
            info->source = TPS25750_PWR_TYPEC_1A5;
            info->available_mv = 5000;
            info->available_ma = 1500;
            break;
        case 0x2:
            info->source = TPS25750_PWR_TYPEC_3A0;
            info->available_mv = 5000;
            info->available_ma = 3000;
            break;
        case 0x3: {
            uint32_t pdo;
            ret = tps25750_read_active_contract_pdo(dev, &pdo);
            if (ret) {
                return ret;
            }
            info->raw_pdo = pdo;
            /* RDO is informational only — don't fail the whole query on it. */
            (void)tps25750_read_active_contract_rdo(dev, &info->raw_rdo);

            /* Fixed-supply PDO (bits 31:30 == 00b): voltage = bits 19:10 in
             * 50mV units, max current = bits 9:0 in 10mA units (TRM Tables
             * 2-22/2-23, per USB PD spec fixed-supply PDO layout). Non-fixed
             * PDOs (variable/battery/PPS) are not decoded — report UNKNOWN
             * with a conservative USB-default budget. */
            if ((pdo >> 30) == 0x0 && pdo != 0) {
                info->source = TPS25750_PWR_PD_CONTRACT;
                info->available_mv = ((pdo >> 10) & 0x3FF) * 50;
                info->available_ma = (pdo & 0x3FF) * 10;
            } else {
                info->source = TPS25750_PWR_UNKNOWN;
                info->available_mv = 5000;
                info->available_ma = 500;
            }
            break;
        }
    }

    return 0;
}

int tps25750_dump(const struct device *dev) {
    if (!dev) {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

#if defined(CONFIG_DUMP_DEVICE_REGISTERS)

    tps25750_mode_t mode;
    int ret = tps25750_read_mode(dev, &mode);
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
#define TPS25750_INT_BIT(_name, _byte, _bit) LOG_INF("EVENT %s: %d", #_name, i._name);

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
    if (ret) {
        LOG_ERR("tps25750_read_device_info: %d", ret);
        return ret;
    }
    LOG_INF("Device Info: %.*s", sizeof(info.str), info.str);

    tps25750_boot_status_t status;
    ret = tps25750_read_boot_status(dev, &status);
    if (ret) {
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

// The 4CC "task" protocol (write DATA1 request -> write CMD1 4CC -> poll CMD1 ->
// read DATA1 result) spans multiple I2C transfers that all share the part's single
// DATA1/CMD1 register pair. Every task sequence must run under this per-device mutex:
// without it, concurrent callers interleave and corrupt each other's request/result
// (observed as I2Cm bridge reads failing with "status" 0x6B == 107 -- the other
// caller's freshly-written target-address byte read back where the result status
// belongs). k_mutex is recursive for the owning thread, and no caller runs in ISR
// context (shell, charger-status thread, BT RX, tps25750 work queue).
static void tps25750_task_lock(const struct device *dev) {
    struct tps25750_dev_data *data = (struct tps25750_dev_data *)dev->data;
    k_mutex_lock(&data->task_mutex, K_FOREVER);
}

static void tps25750_task_unlock(const struct device *dev) {
    struct tps25750_dev_data *data = (struct tps25750_dev_data *)dev->data;
    k_mutex_unlock(&data->task_mutex);
}

static int tps25750_download_patch_locked(const struct device *dev, const char *patch,
                                          uint32_t patchSize) {
    int ret;
    tps25750_data1_t data;
    const struct tps25750_dev_config *cfg = dev->config;

    // Check what mode we are in
    tps25750_mode_t mode;
    ret = tps25750_read_mode(dev, &mode);
    if (ret) {
        LOG_ERR("tps25750_read_mode: %d", ret);
        return ret;
    }

    // If we are in mode == PTCH, we can proceed
    // (fixed misplaced parens on both strncmp calls: the comparisons used to sit inside
    // the length argument, so PTCH was matched on 1 char and APP on 0 chars -- which is
    // why an already-patched boot logged the scary "Cannot download patch!" instead of
    // "Patch already loaded!")
    if (strncmp(mode.mode, TPS25750_REG_MODE_VAL_PTCH, sizeof(mode.mode)) != 0) {
        // Check if we are in APP mode
        if (strncmp(mode.mode, TPS25750_REG_MODE_VAL_APP, sizeof(mode.mode)) == 0) {
            LOG_INF("Patch already loaded!");
            return 0;
        } else {
            LOG_ERR("MODE is not PTCH (got %.*s) Cannot download patch!", sizeof(mode.mode),
                    mode.mode);
            return -EBUSY;
        }
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

    if (patch) {
        // We are actually ready to patch: construct a patch start command
        data.byte_count = 6;
        data.data[0] = (uint8_t)((patchSize >> 0) & 0xFF);   // Byte1 of bundle size
        data.data[1] = (uint8_t)((patchSize >> 8) & 0xFF);   // Byte2 of bundle size
        data.data[2] = (uint8_t)((patchSize >> 16) & 0xFF);  // Byte3 of bundle size
        data.data[3] = (uint8_t)((patchSize >> 24) & 0xFF);  // Byte4 of bundle size
        data.data[4] = cfg->patch_address;  // Slave Address #2, 6 bit. 0x15 chosen at random
        data.data[5] = 0x32;                // Timeout, 0x32 == 5 seconds

        ret = tps25750_write_data1(dev, &data);
        if (ret) {
            LOG_ERR("tps25750_write_data1: %d", ret);
            return ret;
        }

        // Send a "PBMs" command to the device to start the programming sequence
        ret = tps25750_write_cmd1(dev, TPS25750_REG_CMD1_VAL_PBMS);
        if (ret) {
            LOG_ERR("tps25750_write_cmd1: %d", ret);
            return ret;
        }

        // Readback the CMD1 register to check command status
        ret = tps25750_read_cmd_status(dev);
        if (ret) {
            LOG_ERR("tps25750_read_cmd_status: %d", ret);
            return ret;
        } else {
            LOG_INF("Controller accepted PBMs command + data payload!");
        }

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

        // OK! We are ready to patch at this point.

        // Construct a new I2C spec that uses the patch address
        struct i2c_dt_spec i2c_patch = cfg->i2c;
        i2c_patch.addr = cfg->patch_address;

        uint32_t remainingBytes = patchSize;
        const char *writePointer = patch;

        LOG_INF("Starting download of ROM patch");
        LOG_INF("Patch size %u bytes", patchSize);

        do {
            uint32_t writeSize = MIN(cfg->patch_chunk_size, remainingBytes);
            memcpy(cfg->patch_buffer, writePointer, writeSize);

            ret = i2c_write_dt(&i2c_patch, cfg->patch_buffer, writeSize);
            if (ret) {
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
    } else {
        LOG_INF("No patch data. Sending PBMc command...");
    }

    // Send a "PBMc" command to indicate that all patch data has been loaded
    ret = tps25750_write_cmd1(dev, TPS25750_REG_CMD1_VAL_PBMC);
    if (ret) {
        LOG_ERR("tps25750_write_cmd1: %d", ret);
        return ret;
    }

    uint8_t retries = 0;
    do {
        // Wait 50ms... the controller is busy CRC-ing the patch and may NAK
        // Ideally we want to not experience a NAK if we can avoid it
        k_msleep(50);

        // Readback the CMD1 register to check command status
        ret = tps25750_read_cmd_status(dev);
        if (ret) {
            LOG_ERR("PBMC: tps25750_read_cmd_status: %d", ret);
        } else {
            LOG_INF("Controller accepted PBMc command!");
            break;
        }
    } while (ret && (retries++ < 10));

    if (ret) {
        LOG_ERR("Failed reading PBMC command status after %d retries", retries);
        return ret;
    }

    // TI docs say to wait at least 20ms before attempting to read DATA1 register after
    // a patch download
    k_msleep(20);

    // Read the contents of the DATA1 register
    ret = tps25750_read_data1(dev, &data);
    if (ret) {
        LOG_ERR("tps25750_read_data1: %d", ret);
        return ret;
    }

    // Check byte1 of the returned data for status
    if (data.data[0] != 0) {
        LOG_ERR("Patch status was not success! %d", data.data[0]);
        LOG_HEXDUMP_ERR(data.data, sizeof(data.data), "PBMc Return");
        return -EFAULT;
    }

    LOG_INF("Patch download complete! Checking mode...");

    // Check that MODE is now "APP "
    ret = tps25750_read_mode(dev, &mode);
    if (ret) {
        LOG_ERR("tps25750_read_mode: %d", ret);
        return ret;
    }

    // If we are in mode == APP, patch-loading is complete!
    // (fixed misplaced paren: the comparison used to sit inside strncmp's length argument)
    if (strncmp(mode.mode, TPS25750_REG_MODE_VAL_APP, sizeof(mode.mode)) != 0) {
        LOG_ERR("MODE is not APP! Patch download failed!");
        return -EFAULT;
    }

    LOG_INF("Patch complete!");

    return 0;
}

int tps25750_download_patch(const struct device *dev, const char *patch, uint32_t patchSize) {
    if (!dev) {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    // Hold the task mutex for the entire download: an I2Cm bridge access interleaved
    // mid-download would corrupt the PBMs/PBMc sequence (and the bridge is unusable
    // in PTCH mode anyway).
    tps25750_task_lock(dev);
    int ret = tps25750_download_patch_locked(dev, patch, patchSize);
    tps25750_task_unlock(dev);

    return ret;
}

int tps25750_clear_dead_battery(const struct device *dev) {
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

    tps25750_task_lock(dev);

    // Send a "DBfg" command to the device to clear the dead battery flag
    ret = tps25750_write_cmd1(dev, TPS25750_REG_CMD1_VAL_DBFG);
    if (ret) {
        LOG_ERR("tps25750_write_cmd1: %d", ret);
    } else {
        // Readback the CMD1 register to check command status
        ret = tps25750_read_cmd_status(dev);
        if (ret) {
            LOG_ERR("'%s': tps25750_read_cmd_status: %d", TPS25750_REG_CMD1_VAL_DBFG, ret);
        } else {
            LOG_INF("Controller accepted '%s' command!", TPS25750_REG_CMD1_VAL_DBFG);
        }
    }

    tps25750_task_unlock(dev);

    return ret;
}

int tps25750_go2p(const struct device *dev, uint8_t *task_result) {
    if (!dev || !task_result) {
        LOG_ERR("NULL pointer");
        return -ENODEV;
    }

    const struct tps25750_dev_config *cfg = dev->config;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    // Pre-flight context for the operator (plain register read, no task):
    // GO2P's documented reject rule keys off BOOT_STATUS.PatchConfigSource,
    // so log what the part actually reports before issuing the task.
    tps25750_boot_status_t status;
    int ret = tps25750_read_boot_status(dev, &status);
    if (ret) {
        LOG_ERR("tps25750_read_boot_status: %d", ret);
        return ret;
    }
    LOG_WRN("GO2P pre-flight: PatchConfigSource=%u DeadBatteryFlag=%u",
            status.boot_flags[3] >> 5, (status.boot_flags[0] & 0b100) ? 1 : 0);

    tps25750_task_lock(dev);

    // 'GO2P' task, host interface TRM SLVUC05A Table 3-12 (sec. 3.3.5, p.58):
    // forces the PD controller back into PTCH mode to await a patch over I2C.
    // The task takes NO input data (nothing is staged in DATA1); the output is
    // byte 1 of DATA1 = the standard task return code (Table 3-1, p.43:
    // 0=success, 3=rejected). Side effects on success: MODE reads 'PTCH', the
    // USB PD PHY is disabled, and the part may temporarily NAK I2C; the host
    // must then service ReadyForPatch and push the patch "as soon as possible"
    // -- our IRQ work and runtime PTCH recovery both do exactly that.
    //
    // TRM cautions: (a) the task is intended for the ADCINx
    // NegotiateHighVoltage strap option; (b) its reject rule cites
    // PatchConfigSource values 3h/4h, which the same TRM's BOOT_STATUS table
    // (Table 2-15, p.25) marks Reserved (I2C-loaded configs read 6h) -- an
    // apparent TI doc inconsistency, so a clean REJECTED result here is an
    // expected, harmless outcome on this design. Hardware-confirmed
    // 2026-07-17: proto0 reads PatchConfigSource=6 and the part rejects the
    // task (result 3), leaving the bridge fully healthy.
    //
    // This is a test instrument for the PTCH-wedge recovery path (the wedge
    // otherwise needs USB plug/unplug storms to reproduce). Sanctioned caller:
    // the `power pd go2p` shell command, batteries connected.
    ret = tps25750_write_cmd1(dev, TPS25750_REG_CMD1_VAL_GO2P);
    if (ret) {
        LOG_ERR("tps25750_write_cmd1: %d", ret);
    } else {
        ret = tps25750_read_cmd_status(dev);
        if (ret) {
            // An unrecognized 4CC reads back "!CMD" and surfaces as -EBUSY.
            LOG_ERR("'%s': tps25750_read_cmd_status: %d", TPS25750_REG_CMD1_VAL_GO2P, ret);
        } else {
            tps25750_data1_t data;
            ret = tps25750_read_data1(dev, &data);
            if (ret) {
                LOG_ERR("tps25750_read_data1: %d", ret);
            } else {
                *task_result = data.data[0];
                LOG_WRN("GO2P task result: %u (0=accepted, 3=rejected)", data.data[0]);
            }
        }
    }

    tps25750_task_unlock(dev);

    return ret;
}

#define TPS25750_WORKQ_STACK_SIZE 1024
#define TPS25750_WORKQ_PRIORITY 5

/* Kernel-only work queue: K_KERNEL_STACK_* skips the 1KB CONFIG_USERSPACE privileged
 * stack; this stack can never host a K_USER thread. */
K_KERNEL_STACK_DEFINE(tps25750_workq_stack_area, TPS25750_WORKQ_STACK_SIZE);
struct k_work_q tps25750_work_q;

void tps25750_irq_work(struct k_work *item) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(item);
    struct tps25750_dev_data *data = CONTAINER_OF(dwork, struct tps25750_dev_data, work);
    const struct device *dev = data->dev;
    const struct tps25750_dev_config *cfg = dev->config;
    int ret = 0;

    LOG_DBG("dev: %p", dev);
    LOG_DBG("cfg: %p", cfg);
    LOG_DBG("data: %p", data);

    // Figure out what caused the interrupt
    tps25750_int_t interrupt;
    ret = tps25750_read_int_event1(dev, &interrupt);
    if (ret) {
        LOG_ERR("tps25750_read_int_event1: %d", ret);
        // The part may be temporarily NAKing mid self-reset (TRM SLVUC05A Table
        // 3-12). A silent return here used to permanently swallow the edge --
        // the INT line stays asserted, no new edge ever comes, and a PTCH-wedged
        // part was stranded until reboot. Retry a few times instead.
        if (data->irq_retries < TPS25750_RECOVERY_MAX_RETRIES) {
            data->irq_retries++;
            k_work_reschedule_for_queue(&tps25750_work_q, &data->work,
                                        K_MSEC(TPS25750_RECOVERY_RETRY_MS));
        }
        return;
    }
    // irq_retries is deliberately NOT reset here: the nested MODE read below
    // shares the same bound, and resetting after every successful INT_EVENT1
    // read would let a stuck-NAK MODE read retry forever. It resets at the
    // handler's completion points instead.

    // TODO: need to refactor this to handle other kinds of interrupts
    if (interrupt.ReadyForPatch) {
        LOG_INF("TPS25750 IRQ: ReadyForPatch!");

#if defined(CONFIG_TPS25750_INTERNAL_PATCH)
        // Read the current mode
        tps25750_mode_t mode;
        int ret = tps25750_read_mode(data->dev, &mode);
        if (ret) {
            LOG_ERR("tps25750_read_mode: %d", ret);
            // Same transient-NAK reasoning as the read_int_event1 path above.
            if (data->irq_retries < TPS25750_RECOVERY_MAX_RETRIES) {
                data->irq_retries++;
                k_work_reschedule_for_queue(&tps25750_work_q, &data->work,
                                            K_MSEC(TPS25750_RECOVERY_RETRY_MS));
            }
            return;
        }
        data->irq_retries = 0;
        LOG_INF("MODE: %.*s", sizeof(mode.mode), mode.mode);

        // If we are in mode == PTCH, we can proceed
        // (fixed misplaced parens: the comparisons used to sit inside strncmp's length
        // argument -- see the matching note in tps25750_download_patch)
        if (strncmp(mode.mode, TPS25750_REG_MODE_VAL_PTCH, sizeof(mode.mode)) != 0) {
            // Check if we are in APP mode
            if (strncmp(mode.mode, TPS25750_REG_MODE_VAL_APP, sizeof(mode.mode)) == 0) {
                LOG_INF("Patch already loaded!");
                return;
            } else {
                LOG_ERR("MODE is not PTCH (got %.*s) Cannot download patch!", sizeof(mode.mode),
                        mode.mode);
                return;
            }
        }

        const char *patch;
        size_t size;
        ret = tps25750_get_patch(&patch, &size);
        if (ret) {
            LOG_ERR("tps25750_get_patch: %d", ret);
            return;
        }

        ret = tps25750_download_patch(data->dev, patch, size);
        if (ret) {
            LOG_ERR("Patch download failed! %d", ret);
        } else {
            LOG_INF("Patch download success!");
        }
#else
        // No internal patch: nothing to retry, so the transient is over.
        data->irq_retries = 0;
#endif  // CONFIG_TPS25750_INTERNAL_PATCH
    } else {
        data->irq_retries = 0;
    }
}

void tps25750_irq(const struct device *dev, const struct device *port, struct gpio_callback *cb,
                  gpio_port_pins_t pins) {
    struct tps25750_dev_data *data = (struct tps25750_dev_data *)dev->data;

    // Reschedule (not schedule): schedule_for_queue is a no-op while the work is
    // already pending, so during a plug/unplug storm every edge after the first
    // was swallowed and the stale first-edge deadline won. Rescheduling restarts
    // the timer on each edge, giving a true "quiet period" debounce. 250 ms
    // (down from 3 s) still rides out contact bounce while honoring the TRM's
    // requirement to push a patch "as soon as possible" after ReadyForPatch
    // asserts (host interface TRM SLVUC05A Table 3-12).
    k_work_reschedule_for_queue(&tps25750_work_q, &data->work, K_MSEC(250));

    LOG_DBG("Got a TPS25750 callback!");
}

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

// Runtime PTCH recovery: the part can self-reset into PTCH (patch-wait) mode on
// USB plug/unplug transients. In that mode every I2Cm task completes with
// standard task result REJECTED (0x3, host interface TRM SLVUC05A Table 3-1),
// so all bridged BQ25792 traffic fails with -EFAULT while charging continues
// autonomously -- a wedge that used to persist until reboot whenever the
// ReadyForPatch edge was missed (see tps25750_irq_work). This work item is the
// edge-independent healer: triggered by the failure signature itself, it
// re-checks MODE (a plain read) and re-runs the boot-proven patch download.
// It performs no hardware writes beyond that established download sequence.
static void tps25750_recovery_work(struct k_work *item) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(item);
    struct tps25750_dev_data *data = CONTAINER_OF(dwork, struct tps25750_dev_data, recovery_work);
    const struct device *dev = data->dev;

    tps25750_mode_t mode;
    int ret = tps25750_read_mode(dev, &mode);
    if (ret) {
        // Possibly a transient NAK right after the self-reset (TRM SLVUC05A
        // Table 3-12); retry briefly, then go dormant -- the next REJECTED
        // bridge task re-arms recovery. recovery_retries is shared with the
        // bridge callers' tps25750_maybe_schedule_recovery, hence the lock.
        tps25750_task_lock(dev);
        bool retry = data->recovery_retries < TPS25750_RECOVERY_MAX_RETRIES;
        if (retry) {
            data->recovery_retries++;
        }
        tps25750_task_unlock(dev);

        if (retry) {
            k_work_schedule_for_queue(&tps25750_work_q, &data->recovery_work,
                                      K_MSEC(TPS25750_RECOVERY_RETRY_MS));
        } else {
            LOG_ERR("PTCH recovery: tps25750_read_mode: %d (dormant until next reject)", ret);
        }
        return;
    }
    tps25750_task_lock(dev);
    data->recovery_retries = 0;
    tps25750_task_unlock(dev);

    if (strncmp(mode.mode, TPS25750_REG_MODE_VAL_PTCH, sizeof(mode.mode)) != 0) {
        // Not the PTCH wedge -- a task can be rejected for transient reasons in
        // APP mode (e.g. mid PD renegotiation); nothing to heal.
        LOG_DBG("PTCH recovery: MODE is %.*s, nothing to do", sizeof(mode.mode), mode.mode);
        return;
    }

#if defined(CONFIG_TPS25750_INTERNAL_PATCH)
    LOG_WRN("PD controller self-reset to PTCH mode at runtime; re-downloading patch");

    const char *patch;
    size_t size;
    ret = tps25750_get_patch(&patch, &size);
    if (ret) {
        LOG_ERR("tps25750_get_patch: %d", ret);
        return;
    }

    // download_patch holds task_mutex for the entire download and re-checks
    // MODE under the lock, so racing the IRQ-triggered download (same work
    // queue anyway) degenerates to "Patch already loaded!".
    ret = tps25750_download_patch(dev, patch, size);
    if (ret) {
        LOG_ERR("Runtime patch recovery failed: %d", ret);
        k_work_schedule_for_queue(&tps25750_work_q, &data->recovery_work,
                                  K_MSEC(TPS25750_RECOVERY_DEBOUNCE_MS));
    } else {
        LOG_WRN("Runtime patch recovery complete; I2Cm bridge restored");
    }
#else
    LOG_ERR("PD controller is in PTCH mode but no internal patch is available");
#endif  // CONFIG_TPS25750_INTERNAL_PATCH
}

// Called after every bridged I2Cm task, with task_mutex still held (it guards
// last_recovery_ms/recovery_retries against the recovery work item and other
// bridge callers). A task that completed with REJECTED while the driver
// expected it to work is the PTCH-wedge signature; kick the recovery work to
// confirm via MODE and heal. Rate-limited so the charger thread's 500 ms
// polling can't stack patch downloads.
static void tps25750_maybe_schedule_recovery(const struct device *dev, int ret, uint8_t status) {
    struct tps25750_dev_data *data = (struct tps25750_dev_data *)dev->data;

    // Only the DATA1-status branch returns -EFAULT; combined with the recorded
    // status byte this filters out plain bus errors (-EIO), CMD1 timeouts
    // (-ETIMEDOUT) and "!CMD" rejections (-EBUSY).
    if (ret != -EFAULT || status != REJECTED) {
        return;
    }

    int64_t now = k_uptime_get();
    if (now - data->last_recovery_ms < TPS25750_RECOVERY_DEBOUNCE_MS) {
        return;
    }
    data->last_recovery_ms = now;
    data->recovery_retries = 0;

    // Short delay off the caller's thread; no-op if already pending.
    k_work_schedule_for_queue(&tps25750_work_q, &data->recovery_work, K_MSEC(100));
}

static int tps25750_init(const struct device *dev) {
    const struct tps25750_dev_config *cfg = dev->config;
    struct tps25750_dev_data *data = (struct tps25750_dev_data *)dev->data;
    data->dev = dev;

    // Must exist before the first task sequence: the patch download below and the
    // IRQ work queue both run CMD1/DATA1 tasks under this mutex.
    k_mutex_init(&data->task_mutex);

    LOG_INF("dev: %p", dev);
    LOG_INF("cfg: %p", cfg);
    LOG_INF("data: %p", data);

    k_work_queue_init(&tps25750_work_q);
    k_work_queue_start(&tps25750_work_q, tps25750_workq_stack_area,
                       K_KERNEL_STACK_SIZEOF(tps25750_workq_stack_area), TPS25750_WORKQ_PRIORITY,
                       NULL);

    k_work_init_delayable(&data->work, tps25750_irq_work);
    k_work_init_delayable(&data->recovery_work, tps25750_recovery_work);
    // Let the very first REJECTED bridge task schedule recovery immediately,
    // even inside the first TPS25750_RECOVERY_DEBOUNCE_MS of uptime.
    data->last_recovery_ms = -TPS25750_RECOVERY_DEBOUNCE_MS;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

#if defined(CONFIG_TPS25750_COMPRESSED_PATCH_PRELOAD) || defined(CONFIG_TPS25750_INTERNAL_PATCH)
    // ret is only needed by the patch paths; declaring it unguarded trips
    // -Werror=unused-variable when both patch options are off.
    int ret = 0;
    const char *patch;
    size_t size;
    ret = tps25750_get_patch(&patch, &size);
    if (ret) {
        LOG_ERR("tps25750_get_patch: %d", ret);
        return ret;
    }
#endif

#if defined(CONFIG_TPS25750_INTERNAL_PATCH)
    LOG_INF("Patch address: 0x%X", cfg->patch_address);

    ret = tps25750_download_patch(dev, patch, size);
    if (ret) {
        LOG_ERR("Patch download failed! %d", ret);
    } else {
        LOG_INF("Patch download success!");
    }
#endif  // CONFIG_TPS25750_INTERNAL_PATCH

    if (cfg->int_gpio.port) {
        gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(&cfg->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&data->callback, cfg->irq_callback, BIT(cfg->int_gpio.pin));
        gpio_add_callback(cfg->int_gpio.port, &data->callback);

        LOG_INF("TPS25750 interrupt pin configured! Port %s, pin %d", cfg->int_gpio.port->name,
                cfg->int_gpio.pin);
    }

    return 0;
}

static int tps25750_i2cm_write_reg_locked(const struct device *dev, uint8_t addr, uint8_t reg,
                                          uint8_t *dataBuff, uint8_t dataSize) {
    if (dataSize > TPS25750_MAX_I2C_WRITE) {
        LOG_ERR("Cannot write %u bytes: max write length is %u", dataSize, TPS25750_MAX_I2C_WRITE);
        return -ENOMEM;
    }

    tps25750_data1_t data;
    memset(&data, 0, sizeof(data));

    // Clamp dataSize
    dataSize = MIN(dataSize, TPS25750_MAX_I2C_WRITE);

    data.byte_count = 4 + dataSize;
    data.data[0] = addr;
    // TI's amazing datasheets at work: the payloadSize must also include the length of the register
    // address Even though this is a register-write command. And there's no way to ommit the payload
    // address. And that isn't mentioned anywhere in the docs, just this forum thread:
    // https://e2e.ti.com/support/power-management-group/power-management/f/power-management-forum/1097140/bq25792-i2cw-task-can-not-work/4065201?focus=true
    data.data[1] = dataSize + 1;
    // Byte 2: reserved
    data.data[3] = reg;

    if (dataBuff && dataSize > 0) {
        memcpy(&data.data[4], dataBuff, dataSize);
    }

    LOG_HEXDUMP_DBG(&data, data.byte_count + sizeof(data.byte_count), "Writing payload: ");

    int ret = tps25750_write_data1(dev, &data);
    if (ret) {
        LOG_ERR("tps25750_write_data1: %d", ret);
        return ret;
    }

    // Send an "I2Cw" command to the device to send the data out over I2C
    ret = tps25750_write_cmd1(dev, TPS25750_REG_CMD1_VAL_I2CW);
    if (ret) {
        LOG_ERR("tps25750_write_cmd1: %d", ret);
        return ret;
    }

    // Readback the CMD1 register to check command status
    ret = tps25750_read_cmd_status(dev);
    if (ret) {
        LOG_ERR("tps25750_read_cmd_status: %d", ret);
        return ret;
    } else {
        LOG_DBG("Controller accepted I2Cw command!");
    }

    // Read DATA1 to see transaction status
    ret = tps25750_read_data1(dev, &data);
    if (ret) {
        LOG_ERR("tps25750_read_data1: %d", ret);
        return ret;
    }

    // Record the task status byte for the recovery heuristic (see
    // tps25750_maybe_schedule_recovery) -- written under task_mutex.
    ((struct tps25750_dev_data *)dev->data)->last_i2cm_status = data.data[0];

    if (data.data[0] != 0) {
        LOG_ERR("PD Controller I2CM write failure: %u", data.data[0]);
        return -EFAULT;
    }

    return 0;
}

static int tps25750_i2cm_write_reg(const struct device *dev, uint8_t addr, uint8_t reg,
                                   uint8_t *dataBuff, uint8_t dataSize) {
    tps25750_task_lock(dev);
    int ret = tps25750_i2cm_write_reg_locked(dev, addr, reg, dataBuff, dataSize);
    // Still under task_mutex: last_i2cm_status and the recovery bookkeeping
    // (last_recovery_ms, recovery_retries) are all guarded by the same lock.
    tps25750_maybe_schedule_recovery(dev, ret,
                                     ((struct tps25750_dev_data *)dev->data)->last_i2cm_status);
    tps25750_task_unlock(dev);

    return ret;
}

static int tps25750_i2cm_read_reg_locked(const struct device *dev, uint8_t addr, uint8_t reg,
                                         uint8_t *dataBuff, uint8_t dataSize) {
    if (dataSize > TPS25750_MAX_I2C_READ) {
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
    if (ret) {
        LOG_ERR("tps25750_write_data1: %d", ret);
        return ret;
    }

    // Send an "I2Cr" command to the device to send the data out over I2C
    ret = tps25750_write_cmd1(dev, TPS25750_REG_CMD1_VAL_I2CR);
    if (ret) {
        LOG_ERR("tps25750_write_cmd1: %d", ret);
        return ret;
    }

    // Readback the CMD1 register to check command status
    ret = tps25750_read_cmd_status(dev);
    if (ret) {
        LOG_ERR("tps25750_read_cmd_status: %d", ret);
        return ret;
    } else {
        LOG_DBG("Controller accepted " TPS25750_REG_CMD1_VAL_I2CR " command!");
    }

    // Read DATA1 to see transaction status
    ret = tps25750_read_data1(dev, &data);
    if (ret) {
        LOG_ERR("tps25750_read_data1: %d", ret);
        return ret;
    }

    // Record the task status byte for the recovery heuristic (see
    // tps25750_maybe_schedule_recovery) -- written under task_mutex.
    ((struct tps25750_dev_data *)dev->data)->last_i2cm_status = data.data[0];

    if (data.data[0] != 0) {
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

static int tps25750_i2cm_read_reg(const struct device *dev, uint8_t addr, uint8_t reg,
                                  uint8_t *dataBuff, uint8_t dataSize) {
    tps25750_task_lock(dev);
    int ret = tps25750_i2cm_read_reg_locked(dev, addr, reg, dataBuff, dataSize);
    // Still under task_mutex: last_i2cm_status and the recovery bookkeeping
    // (last_recovery_ms, recovery_retries) are all guarded by the same lock.
    tps25750_maybe_schedule_recovery(dev, ret,
                                     ((struct tps25750_dev_data *)dev->data)->last_i2cm_status);
    tps25750_task_unlock(dev);

    return ret;
}

static int i2c_tps25750_i2cm_transfer(const struct device *dev, struct i2c_msg *msgs,
                                      uint8_t num_msgs, uint16_t addr) {
    LOG_DBG("Got I2C master command for address %u, num msgs %u", addr, num_msgs);

    if (num_msgs != 2) {
        LOG_ERR("Unsupported num messages %u", num_msgs);
        return -ENOTSUP;
    }

    // TPS25750 commands don't support 10-bit addressing
    if (msgs[0].flags & I2C_MSG_ADDR_10_BITS || msgs[1].flags & I2C_MSG_ADDR_10_BITS) {
        LOG_ERR("10 bit addressing is not supported");
        return -ENOTSUP;
    }

    // All I2C register transactions start with a write, which we expect to be a single byte.
    // We expect no other flags on the first message
    if (msgs[0].flags != I2C_MSG_WRITE || msgs[0].len != 1) {
        LOG_ERR("Unsupported first transaction: flags %u, len %u", msgs[0].flags, msgs[0].len);
        return -ENOTSUP;
    }

    uint8_t reg_addr = msgs[0].buf[0];

    // Now: is it a register read or a register write?
    if (msgs[1].flags & I2C_MSG_READ) {
        // Register read.
        // We should have the I2C_MSG_RESTART flag here, as well as I2C_MSG_STOP
        if ((msgs[1].flags & I2C_MSG_RESTART) != I2C_MSG_RESTART ||
            (msgs[1].flags & I2C_MSG_STOP) != I2C_MSG_STOP) {
            LOG_ERR("Second I2C message did not have RESTART or STOP flag!");
            return -ENOTSUP;
        }

        return tps25750_i2cm_read_reg(dev, addr, reg_addr, msgs[1].buf, msgs[1].len);
    }

    // Register Write.
    // We should have the I2C_MSG_STOP flag here
    if ((msgs[1].flags & I2C_MSG_STOP) != I2C_MSG_STOP) {
        LOG_ERR("Second I2C message did not have STOP flag!");
        return -ENOTSUP;
    }

    return tps25750_i2cm_write_reg(dev, addr, reg_addr, msgs[1].buf, msgs[1].len);
}
#endif  // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define TPS25750_DEFINE(inst)                                                                      \
    static const struct i2c_driver_api i2c_tps25750_i2cm_driver_api_##inst = {                     \
        .transfer = i2c_tps25750_i2cm_transfer,                                                    \
    };                                                                                             \
    void tps25750_irq_##inst(const struct device *port, struct gpio_callback *cb,                  \
                             gpio_port_pins_t pins) {                                              \
        const struct device *dev = DEVICE_DT_GET(DT_DRV_INST(inst));                               \
        LOG_INF("Got a callback, device %p", dev);                                                 \
        tps25750_irq(dev, port, cb, pins);                                                         \
    }                                                                                              \
                                                                                                   \
    static struct tps25750_dev_data tps25750_data_##inst;                                          \
                                                                                                   \
    static uint8_t tps25750_patch_buffer_##inst[DT_INST_PROP(inst, patch_chunk_size)];             \
                                                                                                   \
    static const struct tps25750_dev_config tps25750_config_##inst = {                             \
        .i2c = I2C_DT_SPEC_INST_GET(inst),                                                         \
        .int_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, irq_gpios, {0}),                                \
        .patch_buffer = tps25750_patch_buffer_##inst,                                              \
        .patch_address = DT_INST_PROP(inst, patch_address),                                        \
        .patch_chunk_size = DT_INST_PROP(inst, patch_chunk_size),                                  \
        .irq_callback = tps25750_irq_##inst};                                                      \
                                                                                                   \
    I2C_DEVICE_DT_INST_DEFINE(inst, tps25750_init, NULL, &tps25750_data_##inst,                    \
                              &tps25750_config_##inst, POST_KERNEL, CONFIG_TPS25750_INIT_PRIORITY, \
                              &i2c_tps25750_i2cm_driver_api_##inst);

DT_INST_FOREACH_STATUS_OKAY(TPS25750_DEFINE)