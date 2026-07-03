#pragma once

#include <extensions/extension_limits.h>
#include <rgbx/rgbx_api.h>

#include <cstddef>
#include <cstdint>

/**
 * @file
 * @brief Pure (kernel-free) validation of untrusted rgbx extension manifests.
 *
 * Everything an extension's manifest claims — including every embedded
 * pointer — is attacker-controlled: a corrupt or hostile .llext could point
 * `name`/`params` anywhere, and a kernel-mode dereference of a wild pointer
 * faults a NON-sandbox thread, halting the whole firmware. This module
 * validates and copies out every field before anything else trusts it.
 *
 * It deliberately has no Zephyr dependencies (the caller supplies the
 * region-membership predicate), so it compiles on native_sim and is covered
 * by the `extensions.manifest` Twister suite.
 */
namespace extension_manifest {

/** @brief Why validation failed (Ok == accepted). */
enum class Result {
    Ok = 0,
    BadManifestPointer,  /**< manifest struct not fully inside the extension */
    BadAbiVersion,       /**< abi_version != RGBX_ABI_VERSION */
    BadDims,             /**< width/height don't match the host display */
    BadParamTable,       /**< param_count > max, or params/count inconsistent
                          *   (params must be NULL iff param_count == 0), or
                          *   table not fully inside the extension */
    BadName,             /**< name pointer outside the extension or not
                          *   NUL-terminated within the scan bound */
    BadParamName,        /**< a param name pointer outside the extension or
                          *   not NUL-terminated within the scan bound */
    BadParamType,        /**< a param type outside enum rgbx_param_type */
    TooManyStringParams, /**< more than RGBX_MAX_STRING_PARAMS string params */
    BadStringDefault,    /**< a string default outside the extension, not
                          *   NUL-terminated, or longer than
                          *   RGBX_PARAM_STRING_MAX-1 bytes */
};

/** @brief Human-readable name for a Result (for log messages). */
const char *result_str(Result r);

/**
 * @brief Region-membership predicate supplied by the caller.
 * @return true if [ptr, ptr+len) lies wholly inside memory the extension
 *         owns (its llext regions on target; a fake window in tests).
 */
using InRegionFn = bool (*)(void *ctx, const void *ptr, size_t len);

/** @brief Validation environment: expected display dims + region predicate. */
struct Env {
    uint32_t expectedWidth;
    uint32_t expectedHeight;
    InRegionFn inRegion;
    void *ctx;
};

/** @brief Sentinel stringSlot value for non-string params. */
inline constexpr uint8_t kNoStringSlot = 0xFF;

/** @brief One validated, copied-out parameter description. */
struct ParamInfo {
    char name[extension_host::kMaxParamNameLen]; /**< always NUL-terminated */
    enum rgbx_param_type type;
    uint32_t defaultValue; /**< scalar types; BOOL clamped to 0/1 */
    uint8_t stringSlot; /**< for STRING params: index into
                         *   rgbx_inputs.param_strings / stringDefaults
                         *   (declaration order among string params);
                         *   kNoStringSlot otherwise */
};

/** @brief Everything the host keeps after validation — no pointers into
 *  extension memory survive. */
struct Metadata {
    char displayName[extension_host::kMaxNameLen]; /**< always NUL-terminated;
                                                    *   "unnamed" if the
                                                    *   manifest name is NULL */
    uint32_t width;
    uint32_t height;
    size_t paramCount;
    size_t stringParamCount;
    ParamInfo params[RGBX_MAX_PARAMS];
    /** String param defaults, indexed by ParamInfo::stringSlot (stored per
     *  string slot, not per param — only RGBX_MAX_STRING_PARAMS of the 16
     *  params can be strings, and this array is replicated per extension
     *  slot). Always NUL-terminated. */
    char stringDefaults[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX];
};

/**
 * @brief Validates an untrusted manifest and copies every field into @p out.
 *
 * Never dereferences any manifest-embedded pointer without first proving it
 * lies inside the extension (via @p env.inRegion, checked byte-wise for
 * strings so a string may end anywhere in a region). Display and param
 * names longer than their buffers are truncated (a NUL must still exist
 * within the scan bound); string DEFAULTS longer than
 * RGBX_PARAM_STRING_MAX-1 are rejected instead, because values must
 * round-trip through BLE unmodified.
 *
 * @param manifest untrusted pointer (already resolved via llext_find_sym or
 *                 a test fixture); may be anywhere — it is bounds-checked
 *                 before the first field read.
 * @param env      expected dims + region predicate.
 * @param out      filled on success (contents undefined on failure).
 * @return Result::Ok or the first failure encountered.
 */
Result validate(const struct rgbx_manifest *manifest, const Env &env, Metadata &out);

}  // namespace extension_manifest
