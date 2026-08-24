#include <extensions/rgbx_package.h>
#include <rgbx/rgbx_v2.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>

#include <cstring>
#include <limits>

namespace rgbx_package {

// The container-format limits (this header) and the RGBX v2 admission profile
// (<rgbx/rgbx_v2.h>) are declared in separate headers. Where they describe the
// same quantity they must not drift, so tie them together at compile time
// wherever the parser is built:
//   * the parameter and string-parameter tables the device stores must hold
//     every slot the v2 profile admits;
//   * a string default's storage is the v2 slot size minus its NUL.
// kMaxWasmBytes is the container's hard format cap and stays larger than the
// v2 memoryless ceiling on purpose: the active policy narrows the accepted
// module to RGBX_V2_MODULE_MAX_BYTES through Policy.maxWasmBytes, and that
// ceiling must fit inside the format cap. This is the reconciliation of the
// 2048 profile ceiling against the 8192 format cap.
static_assert(kMaxParams == RGBX_V2_MAX_PARAMS,
              "device parameter table must match the v2 profile parameter count");
static_assert(kMaxStringParams == RGBX_V2_MAX_STRING_PARAMS,
              "device string-parameter table must match the v2 profile count");
static_assert(kMaxStringValueLen + 1 == RGBX_V2_STRING_PARAM_SIZE,
              "device string storage must match the v2 profile slot size");
static_assert(RGBX_V2_MODULE_MAX_BYTES <= kMaxWasmBytes,
              "the v2 module ceiling must fit within the container format cap");

namespace {

constexpr uint8_t kMagic[] = {'R', 'G', 'B', 'X'};
constexpr uint8_t kWasmHeader[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
constexpr uint32_t kManifestVersion = 1;

uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool addChecked(size_t lhs, size_t rhs, size_t& out) {
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

bool copyPrintableAscii(const zcbor_string& source, char* destination, size_t capacity,
                        bool allowEmpty) {
    if ((!allowEmpty && source.len == 0) || source.len >= capacity) {
        return false;
    }
    for (size_t i = 0; i < source.len; ++i) {
        if (source.value[i] < 0x20 || source.value[i] > 0x7e) {
            return false;
        }
    }
    if (source.len > 0) {
        std::memcpy(destination, source.value, source.len);
    }
    destination[source.len] = '\0';
    return true;
}

bool validExtensionId(const zcbor_string& id, char* destination) {
    if (id.len == 0 || id.len > kMaxExtensionIdLen) {
        return false;
    }
    for (size_t i = 0; i < id.len; ++i) {
        const uint8_t c = id.value[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        const bool separator = i > 0 && (c == '.' || c == '_' || c == '-');
        if (!alnum && !separator) {
            return false;
        }
    }
    std::memcpy(destination, id.value, id.len);
    destination[id.len] = '\0';
    return true;
}

bool decodeUint16(zcbor_state_t* state, uint16_t& value) {
    uint32_t decoded = 0;
    if (!zcbor_uint32_decode(state, &decoded) || decoded > UINT16_MAX) {
        return false;
    }
    value = static_cast<uint16_t>(decoded);
    return true;
}

Result decodeManifest(const uint8_t* data, size_t size, const Policy& policy, Metadata& out) {
    ZCBOR_STATE_D(state, 5, data, size, 1, 0);
    state->constant_state->enforce_canonical = true;

    if (!zcbor_list_start_decode(state)) {
        return Result::BadManifestCbor;
    }

    uint32_t manifestVersion = 0;
    if (!zcbor_uint32_decode(state, &manifestVersion)) {
        return Result::BadManifestCbor;
    }
    if (manifestVersion != kManifestVersion) {
        return Result::UnsupportedManifestVersion;
    }

    zcbor_string string = {};
    if (!zcbor_tstr_decode(state, &string)) {
        return Result::BadManifestCbor;
    }
    if (!validExtensionId(string, out.extensionId)) {
        return Result::BadExtensionId;
    }

    if (!zcbor_tstr_decode(state, &string)) {
        return Result::BadManifestCbor;
    }
    if (!copyPrintableAscii(string, out.displayName, sizeof(out.displayName), false)) {
        return Result::BadDisplayName;
    }

    if (!zcbor_list_start_decode(state) || !decodeUint16(state, out.version.major) ||
        !decodeUint16(state, out.version.minor) || !decodeUint16(state, out.version.patch) ||
        !zcbor_list_end_decode(state)) {
        return Result::BadSemanticVersion;
    }

    if (!zcbor_uint32_decode(state, &out.rgbxAbi) ||
        !zcbor_uint32_decode(state, &out.minimumFirmwareAbi)) {
        return Result::BadManifestCbor;
    }
    if (out.rgbxAbi != policy.rgbxAbi) {
        return Result::AbiMismatch;
    }
    if (out.minimumFirmwareAbi > policy.firmwareAbi) {
        return Result::FirmwareTooOld;
    }

    if (!zcbor_list_start_decode(state) || !zcbor_uint32_decode(state, &out.width) ||
        !zcbor_uint32_decode(state, &out.height) || !zcbor_list_end_decode(state)) {
        return Result::BadManifestCbor;
    }
    if (out.width != policy.width || out.height != policy.height) {
        return Result::GeometryMismatch;
    }

    if (!zcbor_uint32_decode(state, &out.capabilities) ||
        !zcbor_uint32_decode(state, &out.memoryMaxBytes) ||
        !zcbor_uint32_decode(state, &out.budgetClass)) {
        return Result::BadManifestCbor;
    }
    if ((out.capabilities & ~kKnownCapabilities) != 0 ||
        (out.capabilities & ~policy.allowedCapabilities) != 0) {
        return Result::CapabilityMismatch;
    }
    if (out.memoryMaxBytes > policy.maxMemoryBytes) {
        return Result::MemoryLimitExceeded;
    }
    if (out.budgetClass > policy.maxBudgetClass) {
        return Result::BudgetClassUnsupported;
    }

    if (!zcbor_tstr_decode(state, &string) ||
        !copyPrintableAscii(string, out.sourceLanguage, sizeof(out.sourceLanguage), false) ||
        !zcbor_tstr_decode(state, &string) ||
        !copyPrintableAscii(string, out.compilerId, sizeof(out.compilerId), false) ||
        !zcbor_tstr_decode(state, &string) ||
        !copyPrintableAscii(string, out.compilerVersion, sizeof(out.compilerVersion), false)) {
        return Result::BadCompilerMetadata;
    }

    zcbor_string sourceDigest = {};
    if (!zcbor_bstr_decode(state, &sourceDigest)) {
        return Result::BadManifestCbor;
    }
    if (sourceDigest.len != 0 && sourceDigest.len != kDigestSize) {
        return Result::BadSourceDigest;
    }
    out.hasSourceDigest = sourceDigest.len == kDigestSize;
    if (out.hasSourceDigest) {
        std::memcpy(out.sourceDigest, sourceDigest.value, kDigestSize);
    }

    if (!zcbor_list_start_decode(state)) {
        return Result::BadParamTable;
    }
    while (!zcbor_array_at_end(state)) {
        if (out.paramCount >= kMaxParams || !zcbor_list_start_decode(state)) {
            return Result::BadParamTable;
        }

        uint32_t type = 0;
        if (!zcbor_uint32_decode(state, &type) || type > static_cast<uint32_t>(ParamType::String)) {
            return Result::BadParamType;
        }
        ParamInfo& param = out.params[out.paramCount];
        param.type = static_cast<ParamType>(type);

        if (!zcbor_tstr_decode(state, &string)) {
            return Result::BadManifestCbor;
        }
        if (!copyPrintableAscii(string, param.name, sizeof(param.name), false)) {
            return Result::BadParamName;
        }
        for (size_t i = 0; i < out.paramCount; ++i) {
            if (std::strcmp(out.params[i].name, param.name) == 0) {
                return Result::BadParamName;
            }
        }

        switch (param.type) {
            case ParamType::Uint32:
            case ParamType::Color:
                if (!zcbor_uint32_decode(state, &param.scalarDefault)) {
                    return Result::BadParamDefault;
                }
                if (param.type == ParamType::Color && param.scalarDefault > 0x00ffffffu) {
                    return Result::BadParamDefault;
                }
                break;
            case ParamType::Bool: {
                bool value = false;
                if (!zcbor_bool_decode(state, &value)) {
                    return Result::BadParamDefault;
                }
                param.scalarDefault = value ? 1u : 0u;
                break;
            }
            case ParamType::String:
                if (out.stringParamCount >= kMaxStringParams) {
                    return Result::TooManyStringParams;
                }
                if (!zcbor_tstr_decode(state, &string) ||
                    !copyPrintableAscii(string, param.stringDefault, sizeof(param.stringDefault),
                                        true)) {
                    return Result::BadParamDefault;
                }
                ++out.stringParamCount;
                break;
        }

        if (!zcbor_list_end_decode(state)) {
            return Result::BadParamTable;
        }
        ++out.paramCount;
    }
    if (!zcbor_list_end_decode(state) || !zcbor_list_end_decode(state) ||
        !zcbor_payload_at_end(state)) {
        return Result::BadManifestCbor;
    }

    return Result::Ok;
}

}  // namespace

Result validate(const uint8_t* data, size_t size, const Policy& policy, DigestVerifier verifyDigest,
                void* digestContext, PackageView& out) {
    if (data == nullptr) {
        return Result::NullInput;
    }
    if (size < kHeaderSize + kDigestSize) {
        return Result::PackageTooSmall;
    }
    if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
        return Result::BadMagic;
    }
    if (readLe16(data + 4) != kContainerVersion) {
        return Result::UnsupportedContainerVersion;
    }
    if (readLe16(data + 6) != kHeaderSize) {
        return Result::BadHeaderSize;
    }

    const uint32_t manifestSize32 = readLe32(data + 8);
    const uint32_t wasmSize32 = readLe32(data + 12);
    if (readLe32(data + 16) != 0) {
        return Result::UnsupportedFlags;
    }
    if (manifestSize32 == 0 || manifestSize32 > kMaxManifestBytes) {
        return Result::BadManifestSize;
    }
    const size_t maxWasm =
        policy.maxWasmBytes < kMaxWasmBytes ? policy.maxWasmBytes : kMaxWasmBytes;
    if (wasmSize32 < sizeof(kWasmHeader) || wasmSize32 > maxWasm) {
        return Result::BadWasmSize;
    }

    size_t manifestEnd = 0;
    size_t wasmEnd = 0;
    size_t expectedSize = 0;
    if (!addChecked(kHeaderSize, manifestSize32, manifestEnd) ||
        !addChecked(manifestEnd, wasmSize32, wasmEnd) ||
        !addChecked(wasmEnd, kDigestSize, expectedSize) || expectedSize != size) {
        return Result::LengthMismatch;
    }
    if (verifyDigest == nullptr) {
        return Result::DigestVerifierUnavailable;
    }
    if (!verifyDigest(digestContext, data, wasmEnd, data + wasmEnd)) {
        return Result::DigestMismatch;
    }

    PackageView candidate = {};
    candidate.manifest = data + kHeaderSize;
    candidate.manifestSize = manifestSize32;
    candidate.wasm = data + manifestEnd;
    candidate.wasmSize = wasmSize32;

    const Result manifestResult =
        decodeManifest(candidate.manifest, candidate.manifestSize, policy, candidate.metadata);
    if (manifestResult != Result::Ok) {
        return manifestResult;
    }
    if (std::memcmp(candidate.wasm, kWasmHeader, sizeof(kWasmHeader)) != 0) {
        return Result::BadWasmHeader;
    }

    out = candidate;
    return Result::Ok;
}

const char* describe(Result result) {
    switch (result) {
        case Result::Ok:
            return "ok";
        case Result::NullInput:
            return "null input";
        case Result::PackageTooSmall:
            return "package too small";
        case Result::BadMagic:
            return "bad package magic";
        case Result::UnsupportedContainerVersion:
            return "unsupported container version";
        case Result::BadHeaderSize:
            return "bad header size";
        case Result::UnsupportedFlags:
            return "unsupported package flags";
        case Result::BadManifestSize:
            return "bad manifest size";
        case Result::BadWasmSize:
            return "bad Wasm size";
        case Result::LengthMismatch:
            return "package length mismatch";
        case Result::DigestVerifierUnavailable:
            return "digest verifier unavailable";
        case Result::DigestMismatch:
            return "package digest mismatch";
        case Result::BadManifestCbor:
            return "malformed or non-canonical manifest";
        case Result::UnsupportedManifestVersion:
            return "unsupported manifest version";
        case Result::BadExtensionId:
            return "bad extension id";
        case Result::BadDisplayName:
            return "bad display name";
        case Result::BadSemanticVersion:
            return "bad semantic version";
        case Result::AbiMismatch:
            return "RGBX ABI mismatch";
        case Result::FirmwareTooOld:
            return "firmware ABI too old";
        case Result::GeometryMismatch:
            return "display geometry mismatch";
        case Result::CapabilityMismatch:
            return "capability policy mismatch";
        case Result::MemoryLimitExceeded:
            return "memory limit exceeded";
        case Result::BudgetClassUnsupported:
            return "budget class unsupported";
        case Result::BadCompilerMetadata:
            return "bad compiler metadata";
        case Result::BadSourceDigest:
            return "bad source digest";
        case Result::BadParamTable:
            return "bad parameter table";
        case Result::BadParamName:
            return "bad parameter name";
        case Result::BadParamType:
            return "bad parameter type";
        case Result::BadParamDefault:
            return "bad parameter default";
        case Result::TooManyStringParams:
            return "too many string parameters";
        case Result::BadWasmHeader:
            return "bad Wasm header";
    }
    return "unknown";
}

}  // namespace rgbx_package
