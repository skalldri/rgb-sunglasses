/*
 * Issue #380 instrumentation for the PATCHED flashdisk driver copy.
 *
 * Per-disk counters of underlying flash-op failures, so a hardware soak can
 * prove (or rule out) real QSPI erase/program/read errors underneath FatFS —
 * the SDK driver both swallowed the write errors and discarded the errno.
 */

#ifndef FW_DRIVERS_FLASHDISK_FLASHDISK_STATS_H_
#define FW_DRIVERS_FLASHDISK_FLASHDISK_STATS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct flashdisk_patched_stats {
	uint32_t read_errors;    /* flash_read failures (direct reads + cache loads) */
	uint32_t erase_errors;   /* flash_erase failures in cache commit */
	uint32_t program_errors; /* flash_write failures in cache commit */
};

/**
 * @brief Snapshot a disk's flash-op failure counters.
 *
 * @param disk_name Registered disk name (e.g. "NAND").
 * @param out Receives a copy of the counters, taken under the disk lock.
 * @return 0 on success, -ENOENT if no registered flashdisk has that name.
 */
int flashdisk_patched_stats_get(const char *disk_name, struct flashdisk_patched_stats *out);

/**
 * @brief Zero a disk's flash-op failure counters.
 *
 * @param disk_name Registered disk name (e.g. "NAND").
 * @return 0 on success, -ENOENT if no registered flashdisk has that name.
 */
int flashdisk_patched_stats_reset(const char *disk_name);

#ifdef __cplusplus
}
#endif

#endif /* FW_DRIVERS_FLASHDISK_FLASHDISK_STATS_H_ */
