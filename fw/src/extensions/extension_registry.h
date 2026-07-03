#pragma once

#include <cstddef>

/**
 * extension_registry — boot-time enumeration of animation extension files
 * (/NAND:/ext/*.llext), mirroring glim_registry's shape. Entries are sorted
 * by filename so extension animation IDs (0x20 + index, see extension_host)
 * are deterministic for a given file set.
 *
 * Uses fs_* (not syscall-covered) so it must only run from kernel-mode
 * thread context after the FAT mount (i.e. from the pattern controller
 * thread, like glim_registry::init()).
 */
namespace extension_registry {

inline constexpr const char *kDirectory = "/NAND:/ext";
inline constexpr size_t kMaxFiles = 4;
inline constexpr size_t kMaxNameLen = 32;

void init();

size_t count();

/** File name (not path) of entry `index`, or nullptr if out of range. */
const char *name(size_t index);

/** Builds "/NAND:/ext/<name>" into `out`; false if out of range/truncated. */
bool full_path(size_t index, char *out, size_t outLen);

}  // namespace extension_registry
