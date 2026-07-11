#pragma once

#include <extensions/extension_limits.h>
#include <rgbx/rgbx_api.h>

#include <cstddef>
#include <cstdint>

/**
 * @file
 * @brief Pure (Zephyr-free) byte-packer for extension GATT services' bulk
 * metadata characteristic (issue #90).
 *
 * Mirrors the compile-time MetadataBlobBuilder in
 * fw/src/bluetooth/bt_service_cpp.h byte-for-byte — same
 * [version][entry_count][cpf_format][name_len][name_bytes]... wire format,
 * same fixed UUID/version — but built at runtime from data only known after
 * boot-time discovery, since extension identities/param tables aren't known
 * at compile time. Deliberately dependency-free, like extension_manifest.h,
 * so it compiles on native_sim and is covered by a Twister suite.
 */
namespace extension_metadata_blob {

/** @brief Must match kMetadataBlobVersion in bt_service_cpp.h and
 *  METADATA_BLOB_VERSION in app/constants/bluetooth.ts. */
inline constexpr uint8_t kVersion = 1;

/** @brief Animation Name + Is Active + up to RGBX_MAX_PARAMS param characteristics. */
inline constexpr size_t kMaxEntries = 2 + RGBX_MAX_PARAMS;

/** @brief [cpf_format:1][name_len:1][name bytes: up to kMaxParamNameLen-1]. */
inline constexpr size_t kMaxNameLen = extension_host::kMaxParamNameLen - 1;
inline constexpr size_t kMaxEntrySize = 2 + kMaxNameLen;

/** @brief [version:1][entry_count:1] + kMaxEntries * kMaxEntrySize. */
inline constexpr size_t kMaxBlobSize = 2 + kMaxEntries * kMaxEntrySize;

/** @brief Writes the 2-byte header ([version][0]) at buf[0..1]. Caller must
 *  size buf >= kMaxBlobSize. */
void init(uint8_t *buf);

/**
 * @brief Appends one entry ([cpf_format][name_len][name bytes]) at *pos,
 * advancing *pos and returning true, or returns false (leaving buf and *pos
 * unchanged) if it would overflow bufSize — a defensive guard, never
 * expected to trigger given kMaxBlobSize's sizing math. name is truncated to
 * kMaxNameLen bytes (bounded, no strcpy) if a caller ever violates the
 * ParamInfo contract.
 */
bool append(uint8_t *buf, size_t bufSize, size_t *pos, uint8_t cpfFormat, const char *name);

/** @brief Patches buf[1] = entryCount and returns the total blob length (*pos). */
size_t finish(uint8_t *buf, size_t pos, uint8_t entryCount);

}  // namespace extension_metadata_blob
