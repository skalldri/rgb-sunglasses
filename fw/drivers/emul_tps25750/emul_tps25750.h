/*
 * Test-support accessors for the out-of-tree TPS25750 emulator.
 *
 * These let a ztest suite seed the emulated BQ25792 register file behind the
 * I2Cm bridge, shape the CMD1 task timing/outcome, and inspect what the
 * driver sent — without going through the (driver-owned) I2C bus.
 */

#ifndef DRIVERS_EMUL_TPS25750_EMUL_TPS25750_H_
#define DRIVERS_EMUL_TPS25750_EMUL_TPS25750_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/drivers/emul.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FNV-1a 32-bit running hash, shared by the emulator (patch-port receive
 * path) and tests (hashing the expected patch bytes) so the two can be
 * compared without buffering the ~14.6KB patch blob.
 */
#define EMUL_TPS25750_HASH_INIT 2166136261u

static inline uint32_t emul_tps25750_hash_update(uint32_t hash, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

/* Restore runtime defaults: command delay 0, no armed reject/wedge/status,
 * no command pending, BQ register file zeroed. Deliberately preserves MODE
 * and the patch-download statistics so the patch scenario can assert on
 * what happened during driver init.
 */
void emul_tps25750_reset(const struct emul *target);

/* Read/overwrite the emulated BQ25792 register file (the device behind the
 * I2Cm bridge). Multi-byte registers are stored big-endian, matching the
 * BQ25792's wire format (InternalDeviceRegister byte-swaps on read).
 * Raw backdoor access — bypasses the bridge protocol entirely.
 */
int emul_tps25750_get_bq_reg(const struct emul *target, uint8_t reg, uint8_t *out, size_t count);
int emul_tps25750_set_bq_reg(const struct emul *target, uint8_t reg, const uint8_t *in,
                             size_t count);

/* How long CMD1 stays busy (reads back the pending 4CC) after a command is
 * written, before the task executes and CMD1 clears to NUL. Default 0
 * (executes on the first status poll). A nonzero delay forces callers into
 * tps25750_read_cmd_status()'s 10ms sleep — required to open a window for
 * thread interleaving in concurrency tests: with a zero delay a bridged
 * transfer contains no blocking point on native_sim, so threads never
 * interleave and a missing task_mutex would go undetected.
 */
void emul_tps25750_set_cmd_delay_ms(const struct emul *target, uint32_t ms);

/* Arm a one-shot outcome for the NEXT command written to CMD1:
 * - reject: CMD1 completes as "!CMD" (driver sees -EBUSY)
 * - wedge: CMD1 never completes (driver's bounded poll returns -ETIMEDOUT)
 */
void emul_tps25750_arm_cmd_reject(const struct emul *target);
void emul_tps25750_arm_cmd_wedge(const struct emul *target);

/* Arm a one-shot nonzero status byte for the NEXT I2Cr/I2Cw task result
 * (DATA1 byte 0), emulating e.g. a NAK from the bridged BQ25792. The driver
 * reports it as "PD Controller I2CM read failure: <status>" and -EFAULT.
 */
void emul_tps25750_arm_i2cm_status(const struct emul *target, uint8_t status);

/* Current MODE register content (4 chars + NUL terminator). */
void emul_tps25750_get_mode(const struct emul *target, char out[5]);

/* The 4CC most recently written to CMD1 (4 chars + NUL terminator). */
void emul_tps25750_last_4cc(const struct emul *target, char out[5]);

/* Patch-download statistics recorded since power-up: PBMs parameters, byte
 * count received on the patch-port address, and the FNV-1a hash of those
 * bytes (compare against hashing tps25750_get_patch()'s output).
 */
struct emul_tps25750_patch_stats {
    uint32_t announced_size; /* size field from the PBMs request */
    uint8_t patch_address;   /* patch-port address from the PBMs request */
    uint8_t timeout;         /* timeout field from the PBMs request */
    uint32_t rx_count;       /* bytes received on the patch port */
    uint32_t rx_hash;        /* FNV-1a of the received bytes */
    bool pbms_seen;
    bool pbmc_seen;
};

void emul_tps25750_get_patch_stats(const struct emul *target,
                                   struct emul_tps25750_patch_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_EMUL_TPS25750_EMUL_TPS25750_H_ */
