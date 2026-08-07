#pragma once

/**
 * @file
 * @brief Fences MCUmgr's file-management group to the extension directory.
 *
 * The companion app syncs .llext files during an OTA firmware update: it asks
 * for each file's SHA256, compares it against the digest GitHub reports for
 * the matching release asset, and re-uploads the ones that differ. That needs
 * CONFIG_MCUMGR_GRP_FS, which by default grants a bonded peer read AND write
 * access to every path on the FAT disk — including /NAND:/mcuboot.bin, the
 * staging image the bootloader updater flashes from.
 *
 * So the group is enabled together with an MGMT_EVT_OP_FS_MGMT_FILE_ACCESS
 * callback that rejects every operation outside extension_registry::kDirectory.
 * The decision itself is the pure predicate below, kept free of Zephyr mgmt
 * dependencies so it can be unit-tested on native_sim without a BLE stack
 * (suite: extensions.file_transfer).
 */
namespace extension_file_transfer {

/**
 * @brief True if MCUmgr should be allowed to touch @p path.
 *
 * Accepts only a direct child of extension_registry::kDirectory: the path must
 * start with that directory followed by a '/', and the remainder must be a
 * non-empty file name containing no '/' of its own.
 *
 * Rejects, specifically:
 *  - anything outside the directory, including a bare relative file name;
 *  - the directory itself, with or without a trailing separator;
 *  - subdirectories, so the fence can't be widened by creating one;
 *  - any path containing a ".." component. A prefix test alone is not enough
 *    here — FATFS resolves "/NAND:/ext/../mcuboot.bin" perfectly happily, so
 *    a prefix-only check would hand out exactly the access this exists to
 *    deny. Checked over the whole path, not just the tail.
 *
 * Both '/' AND '\' count as separators, because that is what FatFs itself
 * does (ff.c: `IsSeparator(c) ((c) == '/' || (c) == '\\')`). Splitting on '/'
 * alone would accept "/NAND:/ext/sub\secret.bin" as a direct child while
 * FatFs resolved it into a subdirectory.
 *
 * @param path NUL-terminated path from the MCUmgr request; nullptr is denied.
 * @return true to allow the operation, false to reject it.
 */
bool path_allowed(const char *path);

}  // namespace extension_file_transfer
