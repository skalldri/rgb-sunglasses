/**
 * @file
 * @brief Unit tests for the MCUmgr file-access fence
 * (src/extensions/extension_file_transfer.cpp).
 *
 * Enabling CONFIG_MCUMGR_GRP_FS hands a bonded BLE peer read and write access
 * to the whole FAT disk. extension_file_transfer::path_allowed() is the only
 * thing narrowing that back down to the extension directory, so its reject
 * cases are the security property — they get the bulk of the coverage here.
 */

#include <extensions/extension_file_transfer.h>
#include <extensions/extension_registry.h>

#include <zephyr/ztest.h>

using extension_file_transfer::path_allowed;

ZTEST_SUITE(extension_file_transfer, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ accept */

ZTEST(extension_file_transfer, test_accepts_direct_child) {
    zassert_true(path_allowed("/NAND:/ext/plasma.llext"));
    zassert_true(path_allowed("/NAND:/ext/hello.llext"));
}

ZTEST(extension_file_transfer, test_accepts_any_file_name) {
    /* The fence is about location, not about the extension being a .llext —
     * the app decides what to upload. A dotted name must not be confused with
     * a parent-directory component. */
    zassert_true(path_allowed("/NAND:/ext/a"));
    zassert_true(path_allowed("/NAND:/ext/my..ext.llext"));
    zassert_true(path_allowed("/NAND:/ext/..llext"));
    zassert_true(path_allowed("/NAND:/ext/...."));
}

ZTEST(extension_file_transfer, test_accepted_prefix_matches_the_registry_constant) {
    /* Guards against the fence and the directory the host actually scans
     * drifting apart if kDirectory is ever changed. */
    char path[64];
    snprintf(path, sizeof(path), "%s/plasma.llext", extension_registry::kDirectory);
    zassert_true(path_allowed(path));
}

/* ------------------------------------------------------------------ reject */

ZTEST(extension_file_transfer, test_rejects_parent_traversal) {
    /* The case a prefix-only check would let through: FATFS resolves this
     * straight out of the fenced directory. */
    zassert_false(path_allowed("/NAND:/ext/../mcuboot.bin"));
    zassert_false(path_allowed("/NAND:/ext/../../mcuboot.bin"));
    zassert_false(path_allowed("/NAND:/ext/sub/../plasma.llext"));
    zassert_false(path_allowed("/NAND:/ext/.."));
    zassert_false(path_allowed(".."));
}

ZTEST(extension_file_transfer, test_rejects_paths_outside_the_directory) {
    zassert_false(path_allowed("/NAND:/mcuboot.bin"));
    zassert_false(path_allowed("/NAND:/glim/clip.glim"));
    zassert_false(path_allowed("/NAND:/coredump/core_0000.bin"));
    zassert_false(path_allowed("/mcuboot.bin"));
}

ZTEST(extension_file_transfer, test_rejects_the_directory_itself) {
    zassert_false(path_allowed("/NAND:/ext"));
    zassert_false(path_allowed("/NAND:/ext/"));
}

ZTEST(extension_file_transfer, test_rejects_subdirectories) {
    /* Otherwise the fence could be widened simply by creating a directory. */
    zassert_false(path_allowed("/NAND:/ext/sub/plasma.llext"));
    zassert_false(path_allowed("/NAND:/ext/sub/"));
}

ZTEST(extension_file_transfer, test_rejects_prefix_lookalikes) {
    /* "/NAND:/extra/..." shares the directory's characters but is a different
     * directory — a bare strncmp without the separator check would accept it. */
    zassert_false(path_allowed("/NAND:/extra/plasma.llext"));
    zassert_false(path_allowed("/NAND:/ext.bak/plasma.llext"));
}

ZTEST(extension_file_transfer, test_rejects_relative_and_empty_paths) {
    zassert_false(path_allowed("plasma.llext"));
    zassert_false(path_allowed("ext/plasma.llext"));
    zassert_false(path_allowed(""));
    zassert_false(path_allowed(nullptr));
}
