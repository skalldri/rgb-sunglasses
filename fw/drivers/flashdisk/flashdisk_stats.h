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
 * @param out Receives a copy of the counters. Read WITHOUT the disk lock (see
 *            the rationale in flashdisk.c): the fields are independent aligned
 *            uint32_t so they cannot tear, but they are not a snapshot that is
 *            mutually exclusive with an in-flight flash op — a concurrent
 *            failure may or may not be included, and deltas across two calls
 *            are not attributable to any specific I/O.
 * @return 0 on success, -ENOENT if no registered flashdisk has that name.
 */
int flashdisk_patched_stats_get(const char *disk_name, struct flashdisk_patched_stats *out);

/**
 * @brief Zero a disk's flash-op failure counters.
 *
 * Lockless like _get(): a reset racing a concurrent increment can be undone
 * wholesale (the increment's read-modify-write restores the pre-reset total).
 * Only call while no disk I/O is in flight — today's sole caller is the test
 * suite's before-each hook.
 *
 * @param disk_name Registered disk name (e.g. "NAND").
 * @return 0 on success, -ENOENT if no registered flashdisk has that name.
 */
int flashdisk_patched_stats_reset(const char *disk_name);

#ifdef __cplusplus
}
#endif

#endif /* FW_DRIVERS_FLASHDISK_FLASHDISK_STATS_H_ */
