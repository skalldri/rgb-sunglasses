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

/* Writes "<dir>/core_NNNN.bin" (NNNN zero-padded to 4 digits) into out.
 * Returns 0, or -ENOMEM if the result would not fit in cap. */
int format_dump_path(char* out, size_t cap, const char* dir, unsigned int index);

/* ONE directory sweep answering both questions callers have: how many "core_NNNN.bin"
 * files are there, and what is the highest index. Replaces the separate count/max/any
 * helpers, which were three near-identical walks of the same FAT directory — and made
 * drain_to_dir() sweep it twice per pass, on a workqueue whose stack is deliberately
 * small because FATFS calls are stack-hungry.
 *
 * Returns 0 on success. A MISSING directory is reported as -ENOENT with *out_count = 0
 * and *out_max = -1: that is a legitimate "nothing drained yet" and callers may proceed.
 * Any OTHER negative errno means the scan genuinely failed and the counts are unknown —
 * callers must not treat that as "empty", since doing so would reuse an index that
 * collides with an existing dump, or bypass the retention cap on exactly the corrupt
 * volume where it matters most. */
int scan_dumps(const char* dir, int* out_count, int* out_max);

/* Drain a stored dump into a new sequentially-named file under `dir`
 * (created if missing), then invalidate the stored dump.
 *
 * Returns 0 on success, -ENOENT if no dump is stored, -EBADMSG if the stored
 * dump fails verification or doesn't start with the coredump file magic
 * ("ZE"), or a negative errno from the failing filesystem/partition call. On
 * any failure after file creation the partial file is deleted; the stored
 * dump is only invalidated after the file has been written and synced, so a
 * failed drain retries in full on the next pass.
 *
 * `maxFiles` caps how many dumps may accumulate in `dir` (0 = unbounded, the old
 * behaviour). At or above the cap this returns -ENOSPC and does NOT touch the stored
 * dump — so the OLDEST dumps are the ones kept. The cap is evaluated BEFORE the verify
 * and mkdir prologue, so an at-cap board does not re-checksum the capture partition
 * every pass, and does not re-run an fs_mkdir whose -EEXIST the filesystem layer logs
 * as an error unconditionally.
 *
 * That direction is deliberate and is the whole point of the cap. A crash loop
 * produces a first dump that explains the fault and a stream of later ones that are
 * consequences of it, so a "keep newest N" ring would evict precisely the dump worth
 * having. Refusing also keeps /NAND: usable: once the partition fills, extension
 * installs and GLIM writes fail with -ENOSPC, and drain_to_dir()'s own fs_write fails
 * too — the overflow would otherwise destroy the diagnostics that explain it. */
int drain_to_dir(const PartitionOps& ops, const char* dir, int maxFiles = 0);

}  // namespace coredump_manager_core
