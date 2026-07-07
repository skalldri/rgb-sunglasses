#include <extensions/extension_param_persistence.h>

#include <cstring>

namespace extension_param_persistence {

void build_settings_key(char *out, size_t outLen, const char *displayName) {
    if (outLen == 0) {
        return;
    }

    size_t pos = 0;
    static constexpr char kPrefix[] = "ext/";
    for (size_t i = 0; kPrefix[i] != '\0' && pos + 1 < outLen; i++) {
        out[pos++] = kPrefix[i];
    }
    for (size_t i = 0; displayName[i] != '\0' && pos + 1 < outLen; i++) {
        char c = displayName[i];
        out[pos++] = (c == '/' || c == '=') ? '_' : c;
    }
    out[pos] = '\0';
}

uint32_t manifest_fingerprint(const extension_manifest::Metadata &meta) {
    // FNV-1a over the parameter shape. Order-sensitive by construction (each
    // param folds in its type, then its name bytes, then a 0 separator so
    // "ab","c" and "a","bc" can't collide).
    uint32_t h = 2166136261u;
    auto mix = [&h](uint8_t b) {
        h ^= b;
        h *= 16777619u;
    };
    mix(static_cast<uint8_t>(meta.paramCount));
    mix(static_cast<uint8_t>(meta.stringParamCount));
    for (size_t p = 0; p < meta.paramCount; p++) {
        mix(static_cast<uint8_t>(meta.params[p].type));
        for (const char *n = meta.params[p].name; *n != '\0'; n++) {
            mix(static_cast<uint8_t>(*n));
        }
        mix(0);
    }
    return h;
}

void fill_blob(Blob &blob, const extension_manifest::Metadata &meta,
               const uint32_t paramValues[RGBX_MAX_PARAMS],
               const char stringValues[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX]) {
    blob.manifestFingerprint = manifest_fingerprint(meta);
    memcpy(blob.paramValues, paramValues, sizeof(blob.paramValues));
    memcpy(blob.stringValues, stringValues, sizeof(blob.stringValues));
}

void apply_blob(const Blob &blob, const extension_manifest::Metadata &meta,
                uint32_t paramValues[RGBX_MAX_PARAMS],
                char stringValues[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX]) {
    // Discard a blob whose manifest shape no longer matches - applying its values
    // positionally would misassign them to different parameters. Leaves the
    // caller's already-seeded defaults untouched.
    if (blob.manifestFingerprint != manifest_fingerprint(meta)) {
        return;
    }
    for (size_t p = 0; p < meta.paramCount; p++) {
        uint32_t value = blob.paramValues[p];
        if (meta.params[p].type == RGBX_PARAM_BOOL) {
            value = value != 0 ? 1u : 0u;
        }
        paramValues[p] = value;
    }
    for (size_t s = 0; s < meta.stringParamCount; s++) {
        memcpy(stringValues[s], blob.stringValues[s], RGBX_PARAM_STRING_MAX);
        stringValues[s][RGBX_PARAM_STRING_MAX - 1] = '\0';
    }
}

}  // namespace extension_param_persistence
