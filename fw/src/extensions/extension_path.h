#pragma once

/**
 * extension_path — the pure, Zephyr-free path fence shared by every surface
 * that exposes device files over MCUmgr: the fs_mgmt access hook
 * (extension_file_transfer.cpp) and the FILE_MGMT custom group
 * (extension_mgmt.cpp). It lives in its own translation unit, compiled
 * under CONFIG_APP_EXTENSION_HOST, so neither consumer's Kconfig can drop
 * the fence from the link (the hook is gated on APP_EXT_FILE_TRANSFER,
 * the group on APP_EXT_FILE_MANAGEMENT — either may be off).
 *
 * Parameterized by directory so future managed stores (the FILE_MGMT
 * group's "glim" kind) reuse the identical, already-tested logic instead of
 * a second fence. Covered by the `extensions.file_transfer` native_sim
 * suite.
 */
namespace extension_path {

/**
 * @brief True iff @p path names a direct child of @p dir.
 *
 * Rejects: null paths, any ".." path component (component-wise, so a name
 * like "my..ext.llext" passes while "<dir>/../x" does not), paths outside
 * @p dir, the directory itself, trailing separators, and anything nested
 * deeper than one level. Treats '\\' as a separator exactly like FatFs does
 * — splitting on '/' alone would let "<dir>/sub\\x" through the fence and
 * have FatFs resolve it inside a denied subdirectory.
 */
bool within_dir(const char *path, const char *dir);

}  // namespace extension_path
