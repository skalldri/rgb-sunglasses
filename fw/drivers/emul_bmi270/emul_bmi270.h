/*
 * Test-support accessors for the out-of-tree BMI270 emulator.
 *
 * These let a ztest suite peek at the emulated chip's register file and
 * config-load progress without going through the (driver-owned) SPI bus.
 */

#ifndef DRIVERS_EMUL_BMI270_EMUL_BMI270_H_
#define DRIVERS_EMUL_BMI270_EMUL_BMI270_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/drivers/emul.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read `count` bytes of the emulated register file starting at `reg`. */
int emul_bmi270_get_reg(const struct emul *target, uint8_t reg, uint8_t *out, size_t count);

/* Overwrite `count` bytes of the emulated register file starting at `reg`.
 * Bypasses all write side effects (CMD decoding etc.) — raw backdoor access.
 */
int emul_bmi270_set_reg(const struct emul *target, uint8_t reg, const uint8_t *in, size_t count);

/* Total number of bytes the driver has burst-written to BMI270_REG_INIT_DATA
 * since the last (soft) reset — i.e. how much of the feature config file was
 * uploaded. The driver uploads in 256-byte chunks, so this is always a
 * multiple of 256.
 */
size_t emul_bmi270_cfg_bytes_written(const struct emul *target);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_EMUL_BMI270_EMUL_BMI270_H_ */
