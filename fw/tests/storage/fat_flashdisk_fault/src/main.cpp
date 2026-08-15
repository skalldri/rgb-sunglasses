/*
 * Issue #380 regression suite: FatFS over the flashdisk driver with real flash
 * faults injected underneath.
 *
 * Two scenarios build this binary:
 *
 *  - storage.fat_flashdisk_fault (CONFIG_DISK_DRIVER_FLASH_PATCHED=y): the
 *    patched driver copy. A failed flash program/erase must surface as an
 *    error at fs_write/fs_sync, and after an app-style close/reopen/seek
 *    retry the volume must satisfy the fsck invariant (directory size never
 *    exceeds the readable cluster chain).
 *
 *  - storage.fat_flashdisk_fault.sdk_tripwire (CONFIG_DISK_DRIVER_FLASH=y):
 *    the SDK driver, asserting its BUG — disk_flash_access_write() returns 0
 *    on failure, so fs_write reports success while the flash op failed. When
 *    an NCS upgrade ships upstream zephyr commit 81db3fff8f this scenario
 *    FAILS, which is the reminder to delete fw/drivers/flashdisk/ and revert
 *    to the SDK driver.
 */

#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/ztest.h>

#include <ff.h>

#include <climits>
#include <cstring>

#include "fault_flash.h"

#ifdef CONFIG_DISK_DRIVER_FLASH_PATCHED
#include <flashdisk_stats.h>
#endif

namespace {

constexpr size_t kChunk = 4096; /* == FatFS sector == flash erase page, as on proto0 */

FATFS fat_fs;
struct fs_mount_t nand_mnt = {
    .type = FS_FATFS,
    .mnt_point = "/NAND:",
    .fs_data = &fat_fs,
};

/* ELM FAT logical drive id — translate_path() strips the leading '/' from the
 * mount point, same constant storage.cpp uses. */
constexpr const char *kFatDiskId = "NAND:";

void fill_pattern(uint8_t *buf, size_t len, unsigned int seed) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = static_cast<uint8_t>((seed * 131u) ^ (i & 0xFF));
    }
}

/* Each test gets a unique data-pattern base. Without it, a later test writing
 * the same bytes to the same clusters of a re-formatted (but not re-erased)
 * volume matches what is already on media, flashdisk_cache_write()'s memcmp
 * never marks the cache dirty, no flash op is ever issued, and the armed
 * fault is silently never exercised. */
unsigned int test_nonce;

unsigned int chunk_seed(unsigned int i) {
    return test_nonce * 1000u + i;
}

uint8_t chunk_buf[kChunk];

#ifdef CONFIG_DISK_DRIVER_FLASH_PATCHED
/* Helpers used only by the patched-driver-only tests below; guarded so the
 * .sdk_tripwire build (which compiles them out) stays -Werror-clean. */

uint8_t read_buf[kChunk];

/* Mirror of sound.cpp's tap_write_at_retry(): write len bytes at absolute
 * offset pos, recovering from a transient error by close + reopen (clears
 * FatFS's sticky FIL error flag) + seek back. */
int write_at_retry(struct fs_file_t *f, const char *path, off_t pos, const uint8_t *buf,
                   size_t len, int *io_retries) {
    for (int attempt = 0; attempt < 3; attempt++) {
        bool io_ok = true;
        if (attempt > 0 || fs_tell(f) != pos) {
            io_ok = fs_seek(f, pos, FS_SEEK_SET) == 0;
        }
        if (io_ok && fs_write(f, buf, len) == static_cast<ssize_t>(len)) {
            return 0;
        }
        (*io_retries)++;
        fs_close(f);
        if (fs_open(f, path, FS_O_WRITE) < 0) {
            return -EIO; /* same bail-out sound.cpp's original does */
        }
    }
    return -EIO;
}

/* Checked sync with the same recovery shape: a sync failure must not be
 * ignored (f_sync clears FA_MODIFIED even on failure, so a later close would
 * silently succeed — one of the issue #380 laundering paths). */
int sync_with_retry(struct fs_file_t *f, const char *path, off_t end_pos, int *io_retries) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (fs_sync(f) == 0) {
            return 0;
        }
        (*io_retries)++;
        fs_close(f);
        if (fs_open(f, path, FS_O_WRITE) < 0) {
            return -EIO;
        }
        fs_seek(f, end_pos, FS_SEEK_SET);
    }
    return -EIO;
}

/* The fsck invariant, as a read check: the directory-entry size and every one
 * of those bytes being readable through the cluster chain must agree. */
void verify_file_fully_readable(const char *path, size_t expected_size, unsigned int seed_base) {
    struct fs_dirent entry;
    zassert_ok(fs_stat(path, &entry), "stat of %s failed", path);
    zassert_equal(entry.size, expected_size, "dir size %zu != expected %zu (chain lost?)",
                  entry.size, expected_size);

    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_READ));
    size_t total = 0;
    unsigned int seed = seed_base;
    while (true) {
        ssize_t r = fs_read(&f, read_buf, sizeof(read_buf));
        zassert_true(r >= 0, "read failed at offset %zu (%d) — size exceeds chain", total,
                     static_cast<int>(r));
        if (r == 0) {
            break;
        }
        fill_pattern(chunk_buf, static_cast<size_t>(r), seed++);
        zassert_mem_equal(read_buf, chunk_buf, static_cast<size_t>(r),
                          "content mismatch in chunk %u", seed - 1);
        total += static_cast<size_t>(r);
    }
    fs_close(&f);
    zassert_equal(total, expected_size, "readable bytes %zu != dir size %zu", total,
                  expected_size);
}

void remount(void) {
    zassert_ok(fs_unmount(&nand_mnt));
    zassert_ok(fs_mount(&nand_mnt));
}
#endif /* CONFIG_DISK_DRIVER_FLASH_PATCHED */

void fresh_volume_before_each(void *fixture) {
    ARG_UNUSED(fixture);
    test_nonce++;
    fault_flash_disarm();
    if (sys_dnode_is_linked(&nand_mnt.node)) {
        zassert_ok(fs_unmount(&nand_mnt));
    }
    zassert_ok(fs_mkfs(FS_FATFS, reinterpret_cast<uintptr_t>(kFatDiskId), nullptr, 0),
               "mkfs failed");
    zassert_ok(fs_mount(&nand_mnt), "mount failed");
}

void disarm_after_each(void *fixture) {
    ARG_UNUSED(fixture);
    fault_flash_disarm();
}

}  // namespace

ZTEST_SUITE(fat_flashdisk_fault, NULL, NULL, fresh_volume_before_each, disarm_after_each, NULL);

/* The core issue #380 defect and its fix, from FatFS's point of view.
 *
 * Grow a file so FatFS alternates between data pages and the FAT page; every
 * page switch evicts the flashdisk cache (erase + program). With all flash
 * writes failing:
 *  - patched driver: the eviction failure must surface as an fs_write error;
 *  - SDK driver: fs_write keeps reporting success (the bug), and only
 *    fs_sync's CTRL_SYNC — the one propagating path — reports anything.
 */
ZTEST(fat_flashdisk_fault, test_write_error_visibility) {
    const char *path = "/NAND:/t1.bin";
    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC));

    fill_pattern(chunk_buf, kChunk, chunk_seed(0));
    zassert_equal(fs_write(&f, chunk_buf, kChunk), static_cast<ssize_t>(kChunk),
                  "clean baseline write failed");

    fault_flash_arm(FAULT_FLASH_OP_WRITE | FAULT_FLASH_OP_ERASE, UINT_MAX, 0, 0);

    int write_err = 0;
    for (unsigned int i = 1; i <= 8 && write_err == 0; i++) {
        fill_pattern(chunk_buf, kChunk, chunk_seed(i));
        ssize_t w = fs_write(&f, chunk_buf, kChunk);
        if (w < 0) {
            write_err = static_cast<int>(w);
        }
    }
    int sync_rc = fs_sync(&f);

    zassert_true(fault_flash_injected() > 0, "no fault was ever consumed — test is inert");

#ifdef CONFIG_DISK_DRIVER_FLASH_PATCHED
    zassert_true(write_err < 0, "a failed flash program/erase must surface at fs_write");
    struct flashdisk_patched_stats st;
    zassert_ok(flashdisk_patched_stats_get("NAND", &st));
    zassert_true(st.erase_errors + st.program_errors > 0, "instrumentation counted nothing");
    ARG_UNUSED(sync_rc); /* fp->err is sticky; the sync result adds nothing here */
#else
    /* THE SDK BUG (upstream zephyr 81db3fff8f, missing from NCS v3.1.1): all
     * flash-op failures swallowed, fs_write reports success. If this
     * assertion fails, the SDK got the fix — DELETE fw/drivers/flashdisk/
     * and revert CONFIG_DISK_DRIVER_FLASH in the proto0 board conf. */
    zassert_equal(write_err, 0,
                  "fs_write reported an error under the SDK driver: the NCS flashdisk "
                  "appears to be fixed — remove the fw/drivers/flashdisk backport");
    zassert_true(sync_rc < 0, "CTRL_SYNC should still report the stuck dirty page");
#endif

    fault_flash_disarm();
    fs_close(&f);
}

#ifdef CONFIG_DISK_DRIVER_FLASH_PATCHED
/* The end-to-end property issue #380 is actually about: after a transient
 * one-shot program failure mid-capture (erase succeeded, program failed — the
 * page is momentarily blank on media) and the capture path's own recovery
 * (close/reopen/seek retry + checked syncs), the volume must satisfy what
 * fsck checks: the directory size never exceeds the readable cluster chain,
 * before AND after a remount. */
ZTEST(fat_flashdisk_fault, test_dir_size_never_exceeds_chain_after_recovery) {
    const char *path = "/NAND:/cap.bin";
    constexpr unsigned int kChunks = 12; /* 48 KB, a dozen cluster allocations */
    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC));

    int io_retries = 0;
    for (unsigned int i = 0; i < kChunks; i++) {
        fill_pattern(chunk_buf, kChunk, chunk_seed(i));
        if (i == 5) {
            /* One transient program failure, wherever the next commit lands
             * (data, FAT or directory page — all are fair game on hardware). */
            fault_flash_arm(FAULT_FLASH_OP_WRITE, 1, 0, 0);
        }
        zassert_ok(write_at_retry(&f, path, static_cast<off_t>(i) * kChunk, chunk_buf, kChunk,
                                  &io_retries),
                   "write of chunk %u did not recover", i);
        if ((i % 4) == 3) {
            zassert_ok(sync_with_retry(&f, path, static_cast<off_t>(i + 1) * kChunk,
                                       &io_retries),
                       "periodic sync did not recover");
        }
    }

    zassert_ok(sync_with_retry(&f, path, static_cast<off_t>(kChunks) * kChunk, &io_retries),
               "final sync failed");
    zassert_ok(fs_close(&f));

    zassert_equal(fault_flash_injected(), 1, "the one-shot fault never fired — test is inert");
    zassert_true(io_retries > 0, "the fault fired but no retry was needed?");

    struct flashdisk_patched_stats st;
    zassert_ok(flashdisk_patched_stats_get("NAND", &st));
    zassert_true(st.program_errors >= 1, "instrumentation missed the program failure");

    verify_file_fully_readable(path, kChunks * kChunk, chunk_seed(0));
    remount(); /* prove it on media, not in the caches */
    verify_file_fully_readable(path, kChunks * kChunk, chunk_seed(0));
}

/* A fault while OTHER files share the volume must not damage them — the
 * FF_FS_TINY shared window means the evicted page can belong to a file nobody
 * was writing (issue #380's "clean capture, corrupted CSV" observation). */
ZTEST(fat_flashdisk_fault, test_bystander_file_survives_fault) {
    const char *bystander = "/NAND:/keep.bin";
    const char *victim = "/NAND:/churn.bin";
    constexpr unsigned int kBystanderChunks = 3;

    /* Fully written and closed before any fault: must survive verbatim. */
    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, bystander, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC));
    for (unsigned int i = 0; i < kBystanderChunks; i++) {
        fill_pattern(chunk_buf, kChunk, chunk_seed(i));
        zassert_equal(fs_write(&f, chunk_buf, kChunk), static_cast<ssize_t>(kChunk));
    }
    zassert_ok(fs_sync(&f));
    zassert_ok(fs_close(&f));

    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, victim, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC));
    int io_retries = 0;
    for (unsigned int i = 0; i < 8; i++) {
        fill_pattern(chunk_buf, kChunk, chunk_seed(100 + i));
        if (i == 2) {
            fault_flash_arm(FAULT_FLASH_OP_WRITE, 1, 0, 0);
        }
        zassert_ok(write_at_retry(&f, victim, static_cast<off_t>(i) * kChunk, chunk_buf,
                                  kChunk, &io_retries),
                   "victim write %u did not recover", i);
    }
    zassert_ok(sync_with_retry(&f, victim, 8 * static_cast<off_t>(kChunk), &io_retries));
    zassert_ok(fs_close(&f));
    zassert_equal(fault_flash_injected(), 1, "the one-shot fault never fired — test is inert");

    remount();
    verify_file_fully_readable(bystander, kBystanderChunks * kChunk, chunk_seed(0));
}
#endif /* CONFIG_DISK_DRIVER_FLASH_PATCHED */
