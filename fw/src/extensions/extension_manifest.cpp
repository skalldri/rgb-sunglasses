#include <extensions/extension_manifest.h>

#include <cstring>

namespace extension_manifest {
namespace {

/* Names may legitimately sit near the end of a region, so strings are
 * checked byte-wise (inRegion on each byte until the NUL) rather than with
 * one fixed-size range check. The scan bound keeps a missing NUL from
 * walking an entire region. */
constexpr size_t kStringScanMax = 256;

/* Copies a NUL-terminated string from untrusted extension memory into dst
 * (capacity cap, always NUL-terminated on success). Longer strings are
 * truncated to cap-1 bytes, but a NUL must exist within kStringScanMax
 * in-region bytes or the copy is rejected. */
bool copy_untrusted_string(const Env &env, const char *src, char *dst, size_t cap) {
    for (size_t i = 0; i < kStringScanMax; i++) {
        if (!env.inRegion(env.ctx, src + i, 1)) {
            return false;
        }
        if (src[i] == '\0') {
            const size_t n = (i < cap - 1) ? i : cap - 1;
            memcpy(dst, src, n);
            dst[n] = '\0';
            return true;
        }
    }
    return false; /* no NUL within the scan bound */
}

}  // namespace

const char *result_str(Result r) {
    switch (r) {
        case Result::Ok:
            return "ok";
        case Result::BadManifestPointer:
            return "manifest outside extension memory";
        case Result::BadAbiVersion:
            return "ABI version mismatch";
        case Result::BadDims:
            return "framebuffer dims don't match display";
        case Result::BadParamTable:
            return "bad param table";
        case Result::BadName:
            return "bad manifest name";
        case Result::BadParamName:
            return "bad param name";
        case Result::BadParamType:
            return "bad param type";
        case Result::TooManyStringParams:
            return "too many string params";
        case Result::BadStringDefault:
            return "bad string param default";
    }
    return "unknown";
}

Result validate(const struct rgbx_manifest *manifest, const Env &env, Metadata &out) {
    /* Nothing may be read from the manifest before this check. */
    if (manifest == nullptr || !env.inRegion(env.ctx, manifest, sizeof(*manifest))) {
        return Result::BadManifestPointer;
    }

    if (manifest->abi_version != RGBX_ABI_VERSION) {
        return Result::BadAbiVersion;
    }
    if (manifest->width != env.expectedWidth || manifest->height != env.expectedHeight) {
        return Result::BadDims;
    }

    /* params must be NULL iff param_count == 0 (the documented contract),
     * and a non-empty table must lie wholly inside the extension. */
    if (manifest->param_count > RGBX_MAX_PARAMS) {
        return Result::BadParamTable;
    }
    if ((manifest->param_count == 0) != (manifest->params == nullptr)) {
        return Result::BadParamTable;
    }
    if (manifest->param_count > 0 &&
        !env.inRegion(env.ctx, manifest->params,
                      manifest->param_count * sizeof(struct rgbx_param_desc))) {
        return Result::BadParamTable;
    }

    if (manifest->name == nullptr) {
        strncpy(out.displayName, "unnamed", sizeof(out.displayName) - 1);
        out.displayName[sizeof(out.displayName) - 1] = '\0';
    } else if (!copy_untrusted_string(env, manifest->name, out.displayName,
                                      sizeof(out.displayName))) {
        return Result::BadName;
    }

    out.width = manifest->width;
    out.height = manifest->height;
    out.paramCount = manifest->param_count;
    out.stringParamCount = 0;

    for (size_t p = 0; p < manifest->param_count; p++) {
        const struct rgbx_param_desc &desc = manifest->params[p];
        ParamInfo &info = out.params[p];

        if (desc.name == nullptr) {
            strncpy(info.name, "param", sizeof(info.name) - 1);
            info.name[sizeof(info.name) - 1] = '\0';
        } else if (!copy_untrusted_string(env, desc.name, info.name, sizeof(info.name))) {
            return Result::BadParamName;
        }

        info.type = desc.type;
        info.stringSlot = kNoStringSlot;

        switch (desc.type) {
            case RGBX_PARAM_UINT32:
            case RGBX_PARAM_COLOR:
                info.defaultValue = desc.default_value.u32;
                break;
            case RGBX_PARAM_BOOL:
                info.defaultValue = desc.default_value.boolean ? 1u : 0u;
                break;
            case RGBX_PARAM_STRING: {
                if (out.stringParamCount >= RGBX_MAX_STRING_PARAMS) {
                    return Result::TooManyStringParams;
                }
                info.defaultValue = 0;
                char *dst = out.stringDefaults[out.stringParamCount];
                dst[0] = '\0';
                if (desc.default_value.str != nullptr) {
                    /* Defaults must round-trip unmodified, so overlong ones
                     * are rejected rather than truncated: require the copy
                     * to succeed AND to have found the NUL within the value
                     * buffer (strlen < RGBX_PARAM_STRING_MAX). */
                    char tmp[kStringScanMax];
                    if (!copy_untrusted_string(env, desc.default_value.str, tmp,
                                               sizeof(tmp)) ||
                        strlen(tmp) >= RGBX_PARAM_STRING_MAX) {
                        return Result::BadStringDefault;
                    }
                    memcpy(dst, tmp, strlen(tmp) + 1);
                }
                info.stringSlot = static_cast<uint8_t>(out.stringParamCount);
                out.stringParamCount++;
                break;
            }
            default:
                return Result::BadParamType;
        }
    }

    return Result::Ok;
}

}  // namespace extension_manifest
