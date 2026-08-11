#include <zephyr/fs/fs.h>
#include <zephyr/ztest.h>

#include <cerrno>
#include <cstring>

#include <debug/coredump_manager_core.h>

extern "C" {
#include <ff.h>
}

using coredump_manager_core::scan_dumps;
using coredump_manager_core::drain_to_dir;
using coredump_manager_core::format_dump_path;
using coredump_manager_core::PartitionOps;

namespace {

constexpr const char kDumpDir[] = "/NAND:/coredump";

/* ---- Fake coredump partition ------------------------------------------- */
/* Function-pointer ops can't capture, so the fake's state is static. Reset in
 * the suite's before() hook. */
struct FakeState {
    uint8_t dump[4096];
    int size;             // <0 = no dump stored
    bool verifyOk;
    int failCopyAtOffset; // fail the copy() covering this offset (-1 = never)
    int invalidateCalls;
};
FakeState sFake;

void fake_store_dump(int size, uint8_t firstByte = 'Z', uint8_t secondByte = 'E') {
    zassert_true(size <= (int)sizeof(sFake.dump), "fake dump too large");
    for (int i = 0; i < size; i++) {
        sFake.dump[i] = static_cast<uint8_t>(i & 0xFF);
    }
    sFake.dump[0] = firstByte;
    sFake.dump[1] = secondByte;
    sFake.size = size;
}

int fake_has_dump() {
    return sFake.size >= 0 ? 1 : 0;
}

int fake_verify() {
    if (sFake.size < 0) {
        return 0;
    }
    return sFake.verifyOk ? 1 : 0;
}

int fake_get_size() {
    return sFake.size >= 0 ? sFake.size : -ENOENT;
}

int fake_copy(off_t offset, uint8_t* buffer, size_t length) {
    if (sFake.size < 0) {
        return -ENOENT;
    }
    if (sFake.failCopyAtOffset >= 0 && offset <= sFake.failCopyAtOffset &&
        sFake.failCopyAtOffset < (int)(offset + length)) {
        return -EIO;
    }
    size_t n = MIN(length, (size_t)(sFake.size - offset));
    memcpy(buffer, &sFake.dump[offset], n);
    return static_cast<int>(n);
}

int fake_invalidate() {
    sFake.invalidateCalls++;
    sFake.size = -1;
    return 0;
}

constexpr PartitionOps kFakeOps = {
    .has_dump = fake_has_dump,
    .verify = fake_verify,
    .get_size = fake_get_size,
    .copy = fake_copy,
    .invalidate = fake_invalidate,
};

/* ---- FS helpers --------------------------------------------------------- */

/* The three walk helpers collapsed into one scan_dumps() (PR #329 review): it does a
 * single sweep and reports count, max and "the scan failed" separately. These shims keep
 * the assertions below reading the way they did. */
bool anyDumps(const char* dir) {
    int count = 0;
    int maxIndex = -1;
    return scan_dumps(dir, &count, &maxIndex) == 0 && count > 0;
}

int maxDumpIndex(const char* dir, int* out_max) {
    int count = 0;
    return scan_dumps(dir, &count, out_max);
}

int countDumps(const char* dir, int* out_count) {
    int maxIndex = -1;
    return scan_dumps(dir, out_count, &maxIndex);
}

void createEmptyFile(const char* path) {
    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC), "setup: create %s",
               path);
    fs_close(&f);
}

int countFilesIn(const char* dir) {
    struct fs_dir_t dirp;
    fs_dir_t_init(&dirp);
    if (fs_opendir(&dirp, dir) < 0) {
        return 0;
    }
    int count = 0;
    struct fs_dirent entry;
    while (fs_readdir(&dirp, &entry) == 0 && entry.name[0] != '\0') {
        count++;
    }
    fs_closedir(&dirp);
    return count;
}

void removeAllIn(const char* dir) {
    struct fs_dir_t dirp;
    fs_dir_t_init(&dirp);
    if (fs_opendir(&dirp, dir) < 0) {
        return;
    }
    struct fs_dirent entry;
    // entry.name can be up to MAX_FILE_NAME (255) bytes with LFN enabled.
    char path[MAX_FILE_NAME + 32];
    while (fs_readdir(&dirp, &entry) == 0 && entry.name[0] != '\0') {
        int n = snprintf(path, sizeof(path), "%s/%s", dir, entry.name);
        if (n > 0 && (size_t)n < sizeof(path)) {
            fs_unlink(path);
        }
    }
    fs_closedir(&dirp);
}

/* Asserts the file matches the fake dump pattern of `size` bytes. */
void assertFileMatchesDump(const char* path, int size) {
    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_READ), "open %s", path);
    uint8_t buf[256];
    int offset = 0;
    ssize_t n;
    while ((n = fs_read(&f, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            uint8_t expected = (offset + i) == 0   ? 'Z'
                               : (offset + i) == 1 ? 'E'
                                                   : static_cast<uint8_t>((offset + i) & 0xFF);
            zassert_equal(buf[i], expected, "byte %d mismatch", offset + (int)i);
        }
        offset += n;
    }
    fs_close(&f);
    zassert_equal(offset, size, "file size %d != dump size %d", offset, size);
}

/* ---- Suite -------------------------------------------------------------- */

FATFS sNandFat;
struct fs_mount_t sNandMnt = {
    .type = FS_FATFS,
    .mnt_point = "/NAND:",
    .fs_data = &sNandFat,
};

void* suite_setup(void) {
    zassert_ok(fs_mkfs(FS_FATFS, (uintptr_t) "NAND", NULL, 0), "mkfs");
    zassert_ok(fs_mount(&sNandMnt), "mount");
    return NULL;
}

void suite_before(void* fixture) {
    ARG_UNUSED(fixture);
    removeAllIn(kDumpDir);
    memset(&sFake, 0, sizeof(sFake));
    sFake.size = -1;
    sFake.verifyOk = true;
    sFake.failCopyAtOffset = -1;
}

ZTEST_SUITE(coredump_manager_core, NULL, suite_setup, suite_before, NULL, NULL);

/* ---- format_dump_path ---------------------------------------------------- */

ZTEST(coredump_manager_core, test_format_dump_path) {
    char out[64];
    zassert_ok(format_dump_path(out, sizeof(out), kDumpDir, 0));
    zassert_equal(strcmp(out, "/NAND:/coredump/core_0000.bin"), 0, "got %s", out);
    zassert_ok(format_dump_path(out, sizeof(out), kDumpDir, 12345));
    zassert_equal(strcmp(out, "/NAND:/coredump/core_12345.bin"), 0, "got %s", out);
}

ZTEST(coredump_manager_core, test_format_dump_path_too_small) {
    char out[16];
    zassert_equal(format_dump_path(out, sizeof(out), kDumpDir, 0), -ENOMEM);
}

/* ---- scan_dumps ----------------------------------------------------------- */

/* A MISSING directory is distinguished from a BROKEN scan, and reports usable values.
 * This is a deliberate contract change (PR #329 review): drain_to_dir() now scans before
 * it mkdirs, so "not created yet" has to be a normal path — it means index 0 is free and
 * the cap cannot have been reached. A genuinely unreadable directory is a different
 * errno, and drain_to_dir() maps it to -EIO precisely so it cannot be confused with the
 * caller's benign "no dump stored" -ENOENT and silently discard every future dump. */
ZTEST(coredump_manager_core, test_scan_missing_dir_is_enoent_with_empty_counts) {
    int idx = 999;
    int count = 1234;
    zassert_equal(scan_dumps("/NAND:/nonexistent", &count, &idx), -ENOENT);
    zassert_equal(count, 0, "a missing directory holds no dumps");
    zassert_equal(idx, -1, "a missing directory leaves index 0 free");
    zassert_false(anyDumps("/NAND:/nonexistent"));
}

ZTEST(coredump_manager_core, test_max_dump_index_ignores_non_matching) {
    fs_mkdir(kDumpDir);  // ignore -EEXIST from earlier tests
    createEmptyFile("/NAND:/coredump/core_0000.bin");
    createEmptyFile("/NAND:/coredump/core_0042.bin");
    createEmptyFile("/NAND:/coredump/core_.bin");       // no digits
    createEmptyFile("/NAND:/coredump/core_12.txt");     // wrong suffix
    createEmptyFile("/NAND:/coredump/xcore_1.bin");     // wrong prefix
    createEmptyFile("/NAND:/coredump/core_00a1.bin");   // non-digit
    int idx = -1;
    zassert_ok(maxDumpIndex(kDumpDir, &idx));
    zassert_equal(idx, 42);
    zassert_true(anyDumps(kDumpDir));
}

ZTEST(coredump_manager_core, test_any_dump_files_empty_dir) {
    fs_mkdir(kDumpDir);
    zassert_false(anyDumps(kDumpDir));
    int idx = 999;
    zassert_ok(maxDumpIndex(kDumpDir, &idx));
    zassert_equal(idx, -1, "empty dir reports max index -1");
}

/* ---- drain_to_dir --------------------------------------------------------- */

ZTEST(coredump_manager_core, test_drain_no_dump) {
    zassert_equal(drain_to_dir(kFakeOps, kDumpDir), -ENOENT);
    zassert_equal(sFake.invalidateCalls, 0);
}

ZTEST(coredump_manager_core, test_drain_success_multichunk) {
    // > 1024 so the copy loop takes multiple chunks, and not chunk-aligned.
    fake_store_dump(3000);
    zassert_ok(drain_to_dir(kFakeOps, kDumpDir));
    assertFileMatchesDump("/NAND:/coredump/core_0000.bin", 3000);
    zassert_equal(sFake.invalidateCalls, 1, "invalidated exactly once");
    zassert_equal(fake_has_dump(), 0, "partition drained");
    // Second pass: nothing left to drain.
    zassert_equal(drain_to_dir(kFakeOps, kDumpDir), -ENOENT);
}

ZTEST(coredump_manager_core, test_drain_names_sequentially) {
    fs_mkdir(kDumpDir);
    createEmptyFile("/NAND:/coredump/core_0005.bin");
    fake_store_dump(100);
    zassert_ok(drain_to_dir(kFakeOps, kDumpDir));
    assertFileMatchesDump("/NAND:/coredump/core_0006.bin", 100);
}

ZTEST(coredump_manager_core, test_drain_bad_magic_discards) {
    fake_store_dump(500, 'X', 'X');
    zassert_equal(drain_to_dir(kFakeOps, kDumpDir), -EBADMSG);
    // Garbage is discarded so it can't wedge the drain loop forever...
    zassert_equal(sFake.invalidateCalls, 1);
    // ...and no partial file is left behind.
    zassert_false(anyDumps(kDumpDir));
}

ZTEST(coredump_manager_core, test_drain_verify_failure_discards) {
    fake_store_dump(500);
    sFake.verifyOk = false;
    zassert_equal(drain_to_dir(kFakeOps, kDumpDir), -EBADMSG);
    zassert_equal(sFake.invalidateCalls, 1);
    zassert_false(anyDumps(kDumpDir));
}

ZTEST(coredump_manager_core, test_drain_copy_error_keeps_dump_for_retry) {
    fake_store_dump(3000);
    sFake.failCopyAtOffset = 2048;  // fail mid-stream, after some chunks landed
    zassert_equal(drain_to_dir(kFakeOps, kDumpDir), -EIO);
    // The dump survives for a retry and the truncated file was removed.
    zassert_equal(sFake.invalidateCalls, 0);
    zassert_equal(fake_has_dump(), 1);
    zassert_equal(countFilesIn(kDumpDir), 0, "partial file must be deleted");
    // Retry with the fault cleared: drains cleanly.
    sFake.failCopyAtOffset = -1;
    zassert_ok(drain_to_dir(kFakeOps, kDumpDir));
    assertFileMatchesDump("/NAND:/coredump/core_0000.bin", 3000);
}

ZTEST(coredump_manager_core, test_drain_implausibly_small_discards) {
    fake_store_dump(1);
    zassert_equal(drain_to_dir(kFakeOps, kDumpDir), -EBADMSG);
    zassert_equal(sFake.invalidateCalls, 1);
}

}  // namespace

/* ---- retention cap (issue #317) ---- */

ZTEST(coredump_manager_core, test_count_dump_files) {
    fs_mkdir(kDumpDir);
    int count = -1;
    zassert_ok(countDumps(kDumpDir, &count));
    zassert_equal(count, 0, "empty directory should count 0");

    createEmptyFile("/NAND:/coredump/core_0000.bin");
    createEmptyFile("/NAND:/coredump/core_0007.bin");
    // Same non-matching names maxDumpIndex() ignores must not be counted either,
    // or the cap would trip early on unrelated files someone dropped in the directory.
    createEmptyFile("/NAND:/coredump/core_12.txt");
    createEmptyFile("/NAND:/coredump/notes.md");

    zassert_ok(countDumps(kDumpDir, &count));
    zassert_equal(count, 2, "only core_NNNN.bin files count, got %d", count);
}

/* The count and the max come from ONE sweep and must agree with each other — the whole
 * point of collapsing the three former helpers was that drain_to_dir() no longer walks
 * the directory twice per pass. */
ZTEST(coredump_manager_core, test_scan_reports_count_and_max_together) {
    fs_mkdir(kDumpDir);
    createEmptyFile("/NAND:/coredump/core_0003.bin");
    createEmptyFile("/NAND:/coredump/core_0009.bin");
    createEmptyFile("/NAND:/coredump/core_12.txt");  // non-matching: neither counted nor maxed

    int count = 0;
    int maxIndex = -1;
    zassert_ok(scan_dumps(kDumpDir, &count, &maxIndex));
    zassert_equal(count, 2, "got %d", count);
    zassert_equal(maxIndex, 9, "got %d", maxIndex);
}

/* Below the cap, nothing changes. */
ZTEST(coredump_manager_core, test_drain_allowed_below_cap) {
    fs_mkdir(kDumpDir);
    createEmptyFile("/NAND:/coredump/core_0000.bin");
    fake_store_dump(100);

    zassert_ok(drain_to_dir(kFakeOps, kDumpDir, 2), "1 file against a cap of 2 must drain");
    assertFileMatchesDump("/NAND:/coredump/core_0001.bin", 100);
}

/* AT the cap the drain refuses, and — the part that matters — it leaves the stored dump
 * alone and writes no file. The OLDEST dumps survive. */
ZTEST(coredump_manager_core, test_drain_refuses_at_cap_and_keeps_oldest) {
    fs_mkdir(kDumpDir);
    createEmptyFile("/NAND:/coredump/core_0000.bin");
    createEmptyFile("/NAND:/coredump/core_0001.bin");
    fake_store_dump(100);

    zassert_equal(drain_to_dir(kFakeOps, kDumpDir, 2), -ENOSPC);

    // The new dump must NOT have been written...
    struct fs_dirent entry;
    zassert_equal(fs_stat("/NAND:/coredump/core_0002.bin", &entry), -ENOENT,
                  "a refused drain must not create a file");
    // ...and the stored dump must be left intact, so collecting the files and retrying
    // can still rescue it.
    zassert_equal(sFake.invalidateCalls, 0, "a refused drain must not invalidate the dump");

    // The pre-existing (oldest) files are untouched.
    zassert_ok(fs_stat("/NAND:/coredump/core_0000.bin", &entry));
    zassert_ok(fs_stat("/NAND:/coredump/core_0001.bin", &entry));
}

/* Past the cap (files collected out of order, or the cap lowered between builds) must
 * refuse too — the check is >=, not ==. */
ZTEST(coredump_manager_core, test_drain_refuses_above_cap) {
    fs_mkdir(kDumpDir);
    createEmptyFile("/NAND:/coredump/core_0000.bin");
    createEmptyFile("/NAND:/coredump/core_0001.bin");
    createEmptyFile("/NAND:/coredump/core_0002.bin");
    fake_store_dump(100);

    zassert_equal(drain_to_dir(kFakeOps, kDumpDir, 2), -ENOSPC);
}

/* Freeing a slot re-opens the drain, which is what makes coredump-fetch.sh --delete the
 * documented recovery rather than a reflash. */
ZTEST(coredump_manager_core, test_drain_resumes_after_files_are_collected) {
    fs_mkdir(kDumpDir);
    createEmptyFile("/NAND:/coredump/core_0000.bin");
    createEmptyFile("/NAND:/coredump/core_0001.bin");
    fake_store_dump(100);
    zassert_equal(drain_to_dir(kFakeOps, kDumpDir, 2), -ENOSPC);

    zassert_ok(fs_unlink("/NAND:/coredump/core_0000.bin"));
    zassert_ok(drain_to_dir(kFakeOps, kDumpDir, 2), "a freed slot must re-open the drain");
    assertFileMatchesDump("/NAND:/coredump/core_0002.bin", 100);
}

/* maxFiles == 0 keeps the pre-#317 unbounded behaviour, which every other test in this
 * suite relies on by omitting the argument. */
ZTEST(coredump_manager_core, test_cap_of_zero_is_unbounded) {
    fs_mkdir(kDumpDir);
    for (int i = 0; i < 5; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/NAND:/coredump/core_%04d.bin", i);
        createEmptyFile(path);
    }
    fake_store_dump(100);
    zassert_ok(drain_to_dir(kFakeOps, kDumpDir, 0), "cap 0 must mean unbounded");
    assertFileMatchesDump("/NAND:/coredump/core_0005.bin", 100);
}
