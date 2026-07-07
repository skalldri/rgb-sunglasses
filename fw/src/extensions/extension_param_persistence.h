#pragma once

#include <extensions/extension_limits.h>
#include <extensions/extension_manifest.h>
#include <rgbx/rgbx_api.h>

#include <cstddef>

/**
 * @file
 * @brief Pure (Zephyr-free) helpers for persisting extension parameter values
 * via the existing persistent_value_registry/persistent_value_store
 * mechanism (see fw/CLAUDE.md's "Settings-backed config persistence").
 *
 * Deliberately dependency-free, like extension_manifest.h, so it compiles on
 * native_sim and is covered by a Twister suite without pulling in Zephyr/BT.
 */
namespace extension_param_persistence {

/** @brief "ext/" prefix + sanitized display name, always NUL-terminated. */
inline constexpr size_t kKeyMaxLen = 4 + extension_host::kMaxNameLen;

/**
 * @brief Combined persisted payload for one extension: every scalar param
 * value plus every string param value.
 *
 * One entry per extension (not per param), keyed by the extension's stable
 * display name (see build_settings_key()) rather than slot index or
 * declaration order, since slot assignment can shift between boots as
 * /NAND:/ext/ file sets change.
 */
struct Blob {
    /** Order-sensitive fingerprint of the manifest's parameter shape (count +
     * each param's type and name) at save time. apply_blob() discards the blob
     * if this doesn't match the current manifest, so a blob written by a prior
     * .llext version that inserted/removed/reordered/retyped params is never
     * applied positionally to the wrong parameters (see manifest_fingerprint()). */
    uint32_t manifestFingerprint;
    uint32_t paramValues[RGBX_MAX_PARAMS];
    char stringValues[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX];
};

/**
 * @brief Order-sensitive fingerprint over a manifest's parameter shape:
 * paramCount, stringParamCount, and each parameter's type and name.
 *
 * Purely a function of the manifest (never of the values), so the same manifest
 * always fingerprints identically across boots/versions, and any structural
 * change (insert, delete, reorder, retype, rename) changes it. Stamped into a
 * Blob by fill_blob() and re-checked by apply_blob().
 */
uint32_t manifest_fingerprint(const extension_manifest::Metadata &meta);

/**
 * @brief Builds the persistent_value_registry key "ext/<sanitized displayName>"
 * for one extension.
 *
 * displayName is untrusted extension-manifest content (see extension_manifest.h)
 * and may contain '/' or '=', both structurally significant to Zephyr's
 * settings key parser — both are replaced with '_'. Always NUL-terminates
 * @p out, truncating if displayName doesn't fit.
 */
void build_settings_key(char *out, size_t outLen, const char *displayName);

/**
 * @brief Copies paramValues/stringValues into a Blob ready to persist, stamping
 * it with manifest_fingerprint(meta) so a later apply_blob() can detect a manifest
 * change and refuse to apply the blob positionally.
 */
void fill_blob(Blob &blob, const extension_manifest::Metadata &meta,
               const uint32_t paramValues[RGBX_MAX_PARAMS],
               const char stringValues[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX]);

/**
 * @brief Applies a loaded Blob on top of already-defaulted paramValues/stringValues.
 *
 * First checks blob.manifestFingerprint against the current manifest: on any
 * mismatch (params inserted/removed/reordered/retyped/renamed since the blob was
 * saved) the blob is DISCARDED whole — paramValues/stringValues keep their seeded
 * defaults — so stale values are never misassigned to the wrong parameters.
 *
 * On a match it only touches indices meta.paramCount/meta.stringParamCount
 * describe as valid. BOOL params are clamped to 0/1 (mirrors
 * extension_manifest.cpp's validator). Every string is force-NUL-terminated
 * within RGBX_PARAM_STRING_MAX.
 */
void apply_blob(const Blob &blob, const extension_manifest::Metadata &meta,
                uint32_t paramValues[RGBX_MAX_PARAMS],
                char stringValues[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX]);

}  // namespace extension_param_persistence
