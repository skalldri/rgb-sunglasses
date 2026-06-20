#include <zephyr/ztest.h>

#include <storage/glim_registry.h>

#include <zephyr/fs/fs.h>
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

ZTEST_SUITE(glim_registry_di, NULL, nand_fs_setup, NULL, NULL, nand_fs_teardown);

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
