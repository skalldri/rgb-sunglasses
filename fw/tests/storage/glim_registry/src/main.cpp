#include <zephyr/ztest.h>

#include <storage/glim_registry.h>

#include <zephyr/fs/fs.h>
#include <cstdio>
#include <cstring>

extern "C" {
#include <ff.h>
}

namespace {

bool containsName(const char *name) {
    for (size_t i = 0; i < glim_registry::count(); i++) {
        const char *n = glim_registry::name(i);
        if (n && strcmp(n, name) == 0) {
            return true;
        }
    }
    return false;
}

size_t indexOfName(const char *name) {
    for (size_t i = 0; i < glim_registry::count(); i++) {
        const char *n = glim_registry::name(i);
        if (n && strcmp(n, name) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

void createEmptyFile(const char *path) {
    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC), "setup: create %s", path);
    fs_close(&f);
}

}  // namespace

static FATFS s_nand_fat;
static struct fs_mount_t s_nand_mnt = {
    .type = FS_FATFS,
    .mnt_point = "/NAND:",
    .fs_data = &s_nand_fat,
};
static bool s_nand_ready = false;

static void *nand_fs_setup(void) {
    int rc = fs_mkfs(FS_FATFS, (uintptr_t)"NAND", NULL, 0);
    if (rc != 0) {
        return NULL;
    }
    rc = fs_mount(&s_nand_mnt);
    if (rc != 0) {
        return NULL;
    }
    s_nand_ready = true;
    return &s_nand_mnt;
}

static void nand_fs_teardown(void *) {
    if (s_nand_ready) {
        fs_unmount(&s_nand_mnt);
        s_nand_ready = false;
    }
}

// Reformats the filesystem before every test so each test starts from a clean, empty mount
// regardless of execution order (ztest does not guarantee tests run in source-file order) and
// regardless of what an earlier test left behind.
static void nand_fs_before(void *) {
    zassert_true(s_nand_ready, "setup() must have mounted the filesystem");
    zassert_ok(fs_unmount(&s_nand_mnt));
    zassert_ok(fs_mkfs(FS_FATFS, (uintptr_t)"NAND", NULL, 0));
    zassert_ok(fs_mount(&s_nand_mnt));
}

ZTEST_SUITE(glim_registry_di, NULL, nand_fs_setup, nand_fs_before, NULL, nand_fs_teardown);

/* init() must create /NAND:/glim if it doesn't exist yet, and discover files placed inside it
 * (skipping subdirectories), and full_path()/out-of-range accessors must behave correctly. */
ZTEST(glim_registry_di, test_full_lifecycle) {
    // Directory does not exist yet on a freshly formatted filesystem.
    glim_registry::init();
    zassert_equal(glim_registry::count(), 0u, "No files yet, but init() must not crash");

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);
    zassert_ok(fs_opendir(&dir, glim_registry::kDirectory),
              "init() must have created the directory");
    fs_closedir(&dir);

    createEmptyFile("/NAND:/glim/a.glim");
    createEmptyFile("/NAND:/glim/b.glim");
    zassert_ok(fs_mkdir("/NAND:/glim/subdir"), "setup: create subdirectory");

    glim_registry::init();
    zassert_equal(glim_registry::count(), 2u, "Subdirectories must be skipped");
    zassert_true(containsName("a.glim"));
    zassert_true(containsName("b.glim"));

    size_t idxA = indexOfName("a.glim");
    zassert_not_equal(idxA, SIZE_MAX);

    char path[64];
    zassert_true(glim_registry::full_path(idxA, path, sizeof(path)));
    zassert_equal(strcmp(path, "/NAND:/glim/a.glim"), 0, "full_path mismatch: %s", path);

    zassert_is_null(glim_registry::name(glim_registry::count()),
                    "Out-of-range name() must return nullptr");
    zassert_false(glim_registry::full_path(glim_registry::count(), path, sizeof(path)),
                  "Out-of-range full_path() must return false");
}

/* Only files with a ".glim" extension should be discovered - anything else dropped into the
 * directory (READMEs, stray files, etc.) must not show up as a selectable option. */
ZTEST(glim_registry_di, test_filters_non_glim_files) {
    glim_registry::init();

    createEmptyFile("/NAND:/glim/video.glim");
    createEmptyFile("/NAND:/glim/README.txt");
    createEmptyFile("/NAND:/glim/noextension");

    glim_registry::init();
    zassert_equal(glim_registry::count(), 1u, "Only .glim files must be discovered");
    zassert_true(containsName("video.glim"));
    zassert_false(containsName("README.txt"));
    zassert_false(containsName("noextension"));
}

/* Exactly kMaxFiles must fill the table with no truncation reported, and the names
 * must come back sorted. The capacity check used to be a post-write test rather than
 * a precondition, so the boundary was correct only by an invariant nothing stated and
 * nothing exercised. */
ZTEST(glim_registry_di, test_exactly_at_capacity) {
    glim_registry::init();

    char path[64];
    for (size_t i = 0; i < glim_registry::kMaxFiles; i++) {
        snprintf(path, sizeof(path), "/NAND:/glim/f%02zu.glim", i);
        createEmptyFile(path);
    }

    glim_registry::init();
    zassert_equal(glim_registry::count(), glim_registry::kMaxFiles,
                  "a full table must hold exactly kMaxFiles, got %zu", glim_registry::count());
    for (size_t i = 1; i < glim_registry::count(); i++) {
        zassert_true(strcmp(glim_registry::name(i - 1), glim_registry::name(i)) < 0,
                     "names must be sorted ascending: %s then %s", glim_registry::name(i - 1),
                     glim_registry::name(i));
    }
}

/* One file past capacity must not overrun the table, and — the part that matters for
 * slot stability — the RETAINED set must be the alphabetically first kMaxFiles, not
 * whichever kMaxFiles the filesystem happened to hand over first. Slot indices become
 * animation IDs and BLE service UUIDs, so a retained set that varied with FAT order
 * would remap a user's stored selection between boots. */
ZTEST(glim_registry_di, test_one_past_capacity_keeps_alphabetical_prefix) {
    glim_registry::init();

    char path[64];
    // Created in DESCENDING name order, so "first seen" and "alphabetically first"
    // cannot accidentally agree and let a FAT-order-dependent implementation pass.
    for (size_t i = glim_registry::kMaxFiles + 1; i > 0; i--) {
        snprintf(path, sizeof(path), "/NAND:/glim/f%02zu.glim", i - 1);
        createEmptyFile(path);
    }

    glim_registry::init();
    zassert_equal(glim_registry::count(), glim_registry::kMaxFiles,
                  "an over-full directory must clamp to kMaxFiles, got %zu",
                  glim_registry::count());

    for (size_t i = 0; i < glim_registry::kMaxFiles; i++) {
        char expected[32];
        snprintf(expected, sizeof(expected), "f%02zu.glim", i);
        zassert_equal(strcmp(glim_registry::name(i), expected), 0,
                      "slot %zu must be %s, got %s", i, expected, glim_registry::name(i));
    }
    // The one that sorts last is the one dropped.
    char dropped[32];
    snprintf(dropped, sizeof(dropped), "f%02zu.glim", glim_registry::kMaxFiles);
    zassert_false(containsName(dropped), "%s sorts last and must be the entry dropped", dropped);
}

/* init() must be idempotent and must not accumulate across calls — every test above
 * relies on this, and the count reset lives on a different line from the collection. */
ZTEST(glim_registry_di, test_init_is_idempotent) {
    glim_registry::init();
    createEmptyFile("/NAND:/glim/one.glim");
    createEmptyFile("/NAND:/glim/two.glim");

    glim_registry::init();
    const size_t first = glim_registry::count();
    glim_registry::init();
    zassert_equal(glim_registry::count(), first, "re-running init() must not accumulate entries");
    zassert_equal(first, 2u, "expected exactly the two files created");
}
