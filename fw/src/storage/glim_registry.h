#pragma once

#include <cstddef>

// Enumerates .glim files under /NAND:/glim on boot, so the generic GLIM player animation
// (and its BT adapter) can offer a selection without any per-file firmware changes.
namespace glim_registry {

constexpr const char *kDirectory = "/NAND:/glim";
constexpr size_t kMaxFiles = 32;
constexpr size_t kMaxNameLen = 32;  // stored name, truncated; FAT LFN supports up to 255

// Creates kDirectory if missing, then scans it for regular files (skipping subdirectories),
// storing up to kMaxFiles names. Safe to call once at boot, after the FAT filesystem is
// mounted. Logs and leaves the registry empty on any filesystem error.
void init();

size_t count();

// Returns the stored (possibly truncated) file name at index, or nullptr if out of range.
const char *name(size_t index);

// Writes "/NAND:/glim/<name>" for the file at index into out (size outLen). Returns false if
// index is out of range or the path doesn't fit in outLen.
bool full_path(size_t index, char *out, size_t outLen);

}  // namespace glim_registry
