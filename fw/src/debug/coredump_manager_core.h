#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* BT-free, hardware-free logic for draining a stored coredump out of the
 * capture partition into files on the FAT disk (issue #80). All partition
 * access goes through the PartitionOps seam so the logic can be exercised on
 * native_sim (where CONFIG_DEBUG_COREDUMP does not exist) with a fake
 * partition and a real FATFS — see tests/debug/coredump_manager. The thin
 * wiring to the real coredump_query()/coredump_cmd() API lives in
 * coredump_manager.cpp. */

namespace coredump_manager_core {

/* Mirrors the subset of Zephyr's coredump_query()/coredump_cmd() API the
 * manager needs (zephyr/debug/coredump.h). Return conventions match the real
 * API: has_dump/verify return 1 for yes/valid, 0 for no, negative errno on
 * error; copy returns the number of bytes copied (>= 0) or negative errno. */
struct PartitionOps {
    int (*has_dump)();
    int (*verify)();
    int (*get_size)();
    int (*copy)(off_t offset, uint8_t* buffer, size_t length);
    int (*invalidate)();
};

/* Highest NNNN among "core_NNNN.bin" files in `dir`, or -1 if the directory
 * doesn't exist or holds no matching files. Non-matching names are ignored. */
int max_dump_index(const char* dir);

/* Writes "<dir>/core_NNNN.bin" (NNNN zero-padded to 4 digits) into out.
 * Returns 0, or -ENOMEM if the result would not fit in cap. */
int format_dump_path(char* out, size_t cap, const char* dir, unsigned int index);

/* True if `dir` contains at least one "core_*.bin" file. */
bool any_dump_files(const char* dir);

/* Drain a stored dump into a new sequentially-named file under `dir`
 * (created if missing), then invalidate the stored dump.
 *
 * Returns 0 on success, -ENOENT if no dump is stored, -EBADMSG if the stored
 * dump fails verification or doesn't start with the coredump file magic
 * ("ZE"), or a negative errno from the failing filesystem/partition call. On
 * any failure after file creation the partial file is deleted; the stored
 * dump is only invalidated after the file has been written and synced, so a
 * failed drain retries in full on the next pass. */
int drain_to_dir(const PartitionOps& ops, const char* dir);

}  // namespace coredump_manager_core
