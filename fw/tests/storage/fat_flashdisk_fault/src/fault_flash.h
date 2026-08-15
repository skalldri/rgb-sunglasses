/*
 * Control surface for the test-only fault-injecting flash driver
 * (fault_flash.c, DT compatible "test,fault-flash").
 *
 * A fault is armed for an op mask, a shot count and an optional address
 * window; each matching flash op consumes one shot and fails with -EIO,
 * exactly like the nordic,qspi-nor driver reports a WREN/program/erase
 * failure on hardware (issue #380).
 */

#ifndef FW_TESTS_STORAGE_FAT_FLASHDISK_FAULT_SRC_FAULT_FLASH_H_
#define FW_TESTS_STORAGE_FAT_FLASHDISK_FAULT_SRC_FAULT_FLASH_H_

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAULT_FLASH_OP_READ  (1U << 0)
#define FAULT_FLASH_OP_WRITE (1U << 1)
#define FAULT_FLASH_OP_ERASE (1U << 2)

/* Fail the next `count` ops matching `op_mask` that touch
 * [win_start, win_start + win_len); win_len == 0 means any address. */
void fault_flash_arm(unsigned int op_mask, unsigned int count, off_t win_start, size_t win_len);

void fault_flash_disarm(void);

/* Number of ops actually failed since the last arm/disarm reset. */
unsigned int fault_flash_injected(void);

/* Sleep this long at the top of every flash op (0 = off, the default).
 * Purpose: on native_sim a RAM-backed flash op has no blocking point, so
 * concurrent threads never interleave inside FatFS/flashdisk critical
 * sections and a concurrency test proves nothing (see the add-fw-test
 * skill's concurrency note). A small delay creates real preemption windows. */
void fault_flash_set_op_delay_ms(unsigned int ms);

#ifdef __cplusplus
}
#endif

#endif /* FW_TESTS_STORAGE_FAT_FLASHDISK_FAULT_SRC_FAULT_FLASH_H_ */
