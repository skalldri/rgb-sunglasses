/*
 * Emulator for the TI TPS25750 USB-PD controller's I2C host interface.
 *
 * Models the register file the real driver (fw/drivers/tps25750) talks to:
 * MODE, CMD1/DATA1 (the 4CC task protocol), and INT_EVENT1 — plus the two
 * subsystems behind that protocol:
 *
 * - The I2Cm bridge ("I2Cr"/"I2Cw" tasks): serviced against an embedded
 *   BQ25792 register file, so the real bq25792 driver (a devicetree child of
 *   the tps25750 node) runs unmodified on top of the real bridge code.
 * - The patch download ("PBMs"/"PBMc" tasks): chunk writes arrive on a
 *   SECOND I2C address (the DT patch-address property), which EMUL_DT_DEFINE
 *   cannot cover (it keys one emulator to the node's reg address) — so init
 *   hand-registers an extra struct i2c_emul for that address via
 *   i2c_emul_register(). Received bytes are counted + FNV-1a hashed rather
 *   than buffered (the real patch is ~14.6KB).
 *
 * Command timing: a command written to CMD1 stays "busy" (CMD1 reads back
 * the pending 4CC) until emul_tps25750_set_cmd_delay_ms() has elapsed, and
 * executes lazily on the first CMD1 read past the deadline. A nonzero delay
 * is what forces callers into tps25750_read_cmd_status()'s k_msleep(10) poll
 * — the only blocking point in a bridged transfer on native_sim, and thus
 * the thing that makes task_mutex races reproducible in tests.
 *
 * No internal locking: on native_sim, threads only switch at blocking kernel
 * calls, and a transfer callback contains none — each i2c_transfer executes
 * atomically with respect to other threads. Serialization of whole
 * multi-transfer task sequences is the DRIVER's job (task_mutex); the
 * concurrency regression test exists to prove exactly that.
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "../tps25750/tps25750_priv.h"
#include "emul_tps25750.h"

#define DT_DRV_COMPAT ti_tps25750

LOG_MODULE_REGISTER(emul_tps25750, LOG_LEVEL_INF);

/* One-shot outcome armed for the next CMD1 command. */
enum emul_cmd_behavior {
    CMD_BEHAVIOR_NORMAL = 0,
    CMD_BEHAVIOR_REJECT, /* completes as "!CMD" */
    CMD_BEHAVIOR_WEDGE,  /* never completes */
};

struct emul_tps25750_data {
    char mode[TPS25750_REG_MODE_SIZE]; /* "APP " / "PTCH", no NUL */
    char cmd1[TPS25750_REG_CMD1_SIZE]; /* pending 4CC, or all-NUL = idle */
    bool cmd_pending;
    int64_t cmd_issued_at;
    uint32_t cmd_delay_ms;
    enum emul_cmd_behavior cmd_behavior;
    char last_4cc[TPS25750_REG_CMD1_SIZE];

    /* DATA1 as it appears on the wire: [0]=byte_count, [1..64]=payload. */
    uint8_t data1[TPS25750_REG_DATA1_SIZE + 1];

    /* Register file of the BQ25792 behind the I2Cm bridge. Multi-byte
     * registers live here big-endian (the wire format; the BQ driver's
     * InternalDeviceRegister byte-swaps on read). */
    uint8_t bq_regs[0x60];

    uint8_t armed_i2cm_status; /* one-shot nonzero DATA1 status byte */

    /* Patch download state + stats (preserved across emul_tps25750_reset). */
    bool patch_receiving;
    struct emul_tps25750_patch_stats patch_stats;

    /* Second bus presence for the patch-download address. */
    struct i2c_emul patch_port;
};

struct emul_tps25750_cfg {
    uint16_t addr;
    uint8_t patch_address;
};

/* ---- 4CC task execution (runs when CMD1 completes) ---- */

static void stage_response(struct emul_tps25750_data *data, uint8_t status, const uint8_t *payload,
                           size_t len) {
    memset(data->data1, 0, sizeof(data->data1));
    data->data1[0] = (uint8_t)(1 + len); /* byte_count: status + payload */
    data->data1[1] = status;
    if (payload && len > 0) {
        memcpy(&data->data1[2], payload, MIN(len, sizeof(data->data1) - 2));
    }
}

/* Returns false if the 4CC is unknown/invalid in the current state, in which
 * case CMD1 must read back "!CMD" instead of NUL. */
static bool execute_task(struct emul_tps25750_data *data) {
    uint8_t status = data->armed_i2cm_status;

    if (memcmp(data->cmd1, TPS25750_REG_CMD1_VAL_I2CR, TPS25750_REG_CMD1_SIZE) == 0) {
        /* Request: [byte_count=3][addr][reg][len] */
        uint8_t addr = data->data1[1];
        uint8_t reg = data->data1[2];
        uint8_t len = MIN(data->data1[3], TPS25750_MAX_I2C_READ);

        data->armed_i2cm_status = 0;
        if (status == 0 && addr != 0x6B) {
            status = 0x04; /* nothing but the BQ25792 answers on the bridge */
        }
        if (status != 0 || (size_t)reg + len > sizeof(data->bq_regs)) {
            stage_response(data, status != 0 ? status : 0x04, NULL, 0);
        } else {
            stage_response(data, 0, &data->bq_regs[reg], len);
        }
        return true;
    }

    if (memcmp(data->cmd1, TPS25750_REG_CMD1_VAL_I2CW, TPS25750_REG_CMD1_SIZE) == 0) {
        /* Request: [byte_count=4+n][addr][n+1][rsvd][reg][payload x n] */
        uint8_t addr = data->data1[1];
        uint8_t n = data->data1[0] >= 4 ? data->data1[0] - 4 : 0;
        uint8_t reg = data->data1[4];

        data->armed_i2cm_status = 0;
        if (status == 0 && addr != 0x6B) {
            status = 0x04;
        }
        if (status == 0 && (size_t)reg + n <= sizeof(data->bq_regs)) {
            memcpy(&data->bq_regs[reg], &data->data1[5], n);
        }
        stage_response(data, status, NULL, 0);
        return true;
    }

    if (memcmp(data->cmd1, TPS25750_REG_CMD1_VAL_DBFG, TPS25750_REG_CMD1_SIZE) == 0) {
        stage_response(data, 0, NULL, 0);
        return true;
    }

    if (memcmp(data->cmd1, TPS25750_REG_CMD1_VAL_PBMS, TPS25750_REG_CMD1_SIZE) == 0) {
        /* Request: [byte_count=6][size LE32][patch addr][timeout] */
        if (memcmp(data->mode, TPS25750_REG_MODE_VAL_PTCH, TPS25750_REG_MODE_SIZE) != 0) {
            return false;
        }
        data->patch_stats.announced_size = sys_get_le32(&data->data1[1]);
        data->patch_stats.patch_address = data->data1[5];
        data->patch_stats.timeout = data->data1[6];
        data->patch_stats.pbms_seen = true;
        data->patch_stats.rx_count = 0;
        data->patch_stats.rx_hash = EMUL_TPS25750_HASH_INIT;
        data->patch_receiving = true;
        stage_response(data, 0, NULL, 0);
        return true;
    }

    if (memcmp(data->cmd1, TPS25750_REG_CMD1_VAL_PBMC, TPS25750_REG_CMD1_SIZE) == 0) {
        if (!data->patch_receiving) {
            return false;
        }
        data->patch_stats.pbmc_seen = true;
        data->patch_receiving = false;
        if (data->patch_stats.rx_count == data->patch_stats.announced_size) {
            memcpy(data->mode, TPS25750_REG_MODE_VAL_APP, TPS25750_REG_MODE_SIZE);
            stage_response(data, 0, NULL, 0);
        } else {
            stage_response(data, 0x01, NULL, 0);
        }
        return true;
    }

    /* Unknown 4CC (e.g. "GO2P") -> "!CMD", like the real part. */
    return false;
}

/* Lazily complete a pending command on CMD1 read, honoring the busy window
 * and any armed one-shot behavior. */
static void poll_cmd1(struct emul_tps25750_data *data) {
    if (!data->cmd_pending) {
        return;
    }
    if (data->cmd_behavior == CMD_BEHAVIOR_WEDGE) {
        return; /* stays busy forever (until reset) */
    }
    if (k_uptime_get() - data->cmd_issued_at < data->cmd_delay_ms) {
        return; /* still busy */
    }

    bool ok = data->cmd_behavior == CMD_BEHAVIOR_REJECT ? false : execute_task(data);

    if (ok) {
        memset(data->cmd1, 0, sizeof(data->cmd1));
    } else {
        memcpy(data->cmd1, TPS25750_REG_CMD1_VAL_ERROR, TPS25750_REG_CMD1_SIZE);
    }
    data->cmd_behavior = CMD_BEHAVIOR_NORMAL;
    data->cmd_pending = false;
}

/* ---- host-interface register reads/writes ---- */

static int reg_read(struct emul_tps25750_data *data, uint8_t reg, uint8_t *buf, size_t len) {
    memset(buf, 0, len);

    switch (reg) {
        case TPS25750_REG_MODE_ADDR:
            buf[0] = TPS25750_REG_MODE_SIZE;
            memcpy(&buf[1], data->mode, MIN(len - 1, TPS25750_REG_MODE_SIZE));
            return 0;
        case TPS25750_REG_CMD1_ADDR:
            poll_cmd1(data);
            buf[0] = TPS25750_REG_CMD1_SIZE;
            memcpy(&buf[1], data->cmd1, MIN(len - 1, TPS25750_REG_CMD1_SIZE));
            return 0;
        case TPS25750_REG_DATA1_ADDR:
            memcpy(buf, data->data1, MIN(len, sizeof(data->data1)));
            return 0;
        case TPS25750_REG_INT_EVENT1_ADDR:
            buf[0] = TPS25750_REG_INT_SIZE;
            /* ReadyForPatch (byte 10, bit 1) while awaiting a patch. */
            if (len > 11 &&
                memcmp(data->mode, TPS25750_REG_MODE_VAL_PTCH, TPS25750_REG_MODE_SIZE) == 0) {
                buf[1 + 10] |= BIT(1);
            }
            return 0;
        default:
            LOG_ERR("read of unmodeled register 0x%02X", reg);
            return -EIO;
    }
}

static int reg_write(struct emul_tps25750_data *data, uint8_t reg, const uint8_t *buf, size_t len) {
    switch (reg) {
        case TPS25750_REG_CMD1_ADDR:
            /* [byte_count=4][4CC] */
            if (len < 1 + TPS25750_REG_CMD1_SIZE) {
                return -EIO;
            }
            memcpy(data->cmd1, &buf[1], TPS25750_REG_CMD1_SIZE);
            memcpy(data->last_4cc, &buf[1], TPS25750_REG_CMD1_SIZE);
            data->cmd_pending = true;
            data->cmd_issued_at = k_uptime_get();
            return 0;
        case TPS25750_REG_DATA1_ADDR:
            memset(data->data1, 0, sizeof(data->data1));
            memcpy(data->data1, buf, MIN(len, sizeof(data->data1)));
            return 0;
        case TPS25750_REG_INT_CLEAR1_ADDR:
            return 0; /* accepted, nothing modeled to clear */
        default:
            LOG_ERR("write to unmodeled register 0x%02X", reg);
            return -EIO;
    }
}

/* ---- i2c_emul transfer entry points ---- */

static int emul_tps25750_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
                                  int addr) {
    struct emul_tps25750_data *data = target->data;

    ARG_UNUSED(addr);

    /* Both i2c_burst_read_dt (write reg + read) and i2c_burst_write_dt
     * (write reg + write payload) arrive as exactly two messages whose
     * first is a 1-byte register write. */
    if (num_msgs != 2 || (msgs[0].flags & I2C_MSG_READ) || msgs[0].len != 1) {
        LOG_ERR("unsupported message shape (num_msgs %d)", num_msgs);
        return -EIO;
    }

    uint8_t reg = msgs[0].buf[0];

    if (msgs[1].flags & I2C_MSG_READ) {
        return reg_read(data, reg, msgs[1].buf, msgs[1].len);
    }
    return reg_write(data, reg, msgs[1].buf, msgs[1].len);
}

/* Patch-port address: the driver streams raw patch chunks here with plain
 * i2c_write_dt (single write message, no register byte). */
static int emul_tps25750_patch_port_transfer(const struct emul *target, struct i2c_msg *msgs,
                                             int num_msgs, int addr) {
    struct emul_tps25750_data *data = target->data;

    ARG_UNUSED(addr);

    if (!data->patch_receiving) {
        LOG_ERR("patch-port write outside a PBMs/PBMc window");
        return -EIO;
    }

    for (int i = 0; i < num_msgs; i++) {
        if (msgs[i].flags & I2C_MSG_READ) {
            return -EIO;
        }
        data->patch_stats.rx_hash =
            emul_tps25750_hash_update(data->patch_stats.rx_hash, msgs[i].buf, msgs[i].len);
        data->patch_stats.rx_count += msgs[i].len;
    }
    return 0;
}

static const struct i2c_emul_api emul_tps25750_api_i2c = {
    .transfer = emul_tps25750_transfer,
};

static const struct i2c_emul_api emul_tps25750_patch_port_api = {
    .transfer = emul_tps25750_patch_port_transfer,
};

/* ---- test backdoors (emul_tps25750.h) ---- */

void emul_tps25750_reset(const struct emul *target) {
    struct emul_tps25750_data *data = target->data;

    data->cmd_pending = false;
    data->cmd_delay_ms = 0;
    data->cmd_behavior = CMD_BEHAVIOR_NORMAL;
    data->armed_i2cm_status = 0;
    memset(data->cmd1, 0, sizeof(data->cmd1));
    memset(data->data1, 0, sizeof(data->data1));
    memset(data->bq_regs, 0, sizeof(data->bq_regs));
    /* mode + patch_stats intentionally preserved (boot-time artifacts). */
}

int emul_tps25750_get_bq_reg(const struct emul *target, uint8_t reg, uint8_t *out, size_t count) {
    struct emul_tps25750_data *data = target->data;

    if ((size_t)reg + count > sizeof(data->bq_regs)) {
        return -EINVAL;
    }
    memcpy(out, &data->bq_regs[reg], count);
    return 0;
}

int emul_tps25750_set_bq_reg(const struct emul *target, uint8_t reg, const uint8_t *in,
                             size_t count) {
    struct emul_tps25750_data *data = target->data;

    if ((size_t)reg + count > sizeof(data->bq_regs)) {
        return -EINVAL;
    }
    memcpy(&data->bq_regs[reg], in, count);
    return 0;
}

void emul_tps25750_set_cmd_delay_ms(const struct emul *target, uint32_t ms) {
    struct emul_tps25750_data *data = target->data;

    data->cmd_delay_ms = ms;
}

void emul_tps25750_arm_cmd_reject(const struct emul *target) {
    struct emul_tps25750_data *data = target->data;

    data->cmd_behavior = CMD_BEHAVIOR_REJECT;
}

void emul_tps25750_arm_cmd_wedge(const struct emul *target) {
    struct emul_tps25750_data *data = target->data;

    data->cmd_behavior = CMD_BEHAVIOR_WEDGE;
}

void emul_tps25750_arm_i2cm_status(const struct emul *target, uint8_t status) {
    struct emul_tps25750_data *data = target->data;

    data->armed_i2cm_status = status;
}

void emul_tps25750_get_mode(const struct emul *target, char out[5]) {
    struct emul_tps25750_data *data = target->data;

    memcpy(out, data->mode, TPS25750_REG_MODE_SIZE);
    out[4] = '\0';
}

void emul_tps25750_last_4cc(const struct emul *target, char out[5]) {
    struct emul_tps25750_data *data = target->data;

    memcpy(out, data->last_4cc, TPS25750_REG_CMD1_SIZE);
    out[4] = '\0';
}

void emul_tps25750_get_patch_stats(const struct emul *target,
                                   struct emul_tps25750_patch_stats *out) {
    struct emul_tps25750_data *data = target->data;

    *out = data->patch_stats;
}

/* ---- registration ---- */

static int emul_tps25750_init(const struct emul *target, const struct device *parent) {
    struct emul_tps25750_data *data = target->data;
    const struct emul_tps25750_cfg *cfg = target->cfg;

    memcpy(data->mode,
           IS_ENABLED(CONFIG_EMUL_TPS25750_BOOT_MODE_PTCH) ? TPS25750_REG_MODE_VAL_PTCH
                                                           : TPS25750_REG_MODE_VAL_APP,
           TPS25750_REG_MODE_SIZE);
    data->patch_stats.rx_hash = EMUL_TPS25750_HASH_INIT;

    /* Second presence on the bus for the patch-download address (see the
     * file comment). target points back at this emulator so the patch-port
     * transfer handler shares this data struct. */
    data->patch_port.addr = cfg->patch_address;
    data->patch_port.api = &emul_tps25750_patch_port_api;
    data->patch_port.target = target;
    return i2c_emul_register(parent, &data->patch_port);
}

#define EMUL_TPS25750_DEFINE(n)                                                       \
    static struct emul_tps25750_data emul_tps25750_data_##n;                          \
    static const struct emul_tps25750_cfg emul_tps25750_cfg_##n = {                   \
        .addr = DT_INST_REG_ADDR(n),                                                  \
        .patch_address = DT_INST_PROP(n, patch_address),                              \
    };                                                                                \
    EMUL_DT_INST_DEFINE(n, emul_tps25750_init, &emul_tps25750_data_##n,               \
                        &emul_tps25750_cfg_##n, &emul_tps25750_api_i2c, NULL)

DT_INST_FOREACH_STATUS_OKAY(EMUL_TPS25750_DEFINE)
