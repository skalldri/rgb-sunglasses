#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file
 * @brief Allocation-free validation of an untrusted RGBX WebAssembly package.
 *
 * This parser owns no filesystem, crypto, registry, or runtime side effects.
 * The caller supplies a digest verifier and receives copied metadata plus
 * read-only spans into the input package only after every check succeeds.
 */
namespace rgbx_package {

inline constexpr uint16_t kContainerVersion = 1;
inline constexpr uint16_t kHeaderSize = 20;
inline constexpr size_t kDigestSize = 32;
inline constexpr size_t kMaxManifestBytes = 2048;
inline constexpr size_t kMaxWasmBytes = 8192;
inline constexpr size_t kMaxExtensionIdLen = 31;
inline constexpr size_t kMaxDisplayNameLen = 31;
inline constexpr size_t kMaxParamNameLen = 19;
inline constexpr size_t kMaxStringValueLen = 31;
inline constexpr size_t kMaxParams = 16;
inline constexpr size_t kMaxStringParams = 4;
inline constexpr size_t kMaxSourceLanguageLen = 15;
inline constexpr size_t kMaxCompilerIdLen = 31;
inline constexpr size_t kMaxCompilerVersionLen = 15;

enum Capability : uint32_t {
    CapabilityButtons = 1u << 0,
    CapabilityImu = 1u << 1,
    CapabilityAudio = 1u << 2,
};

inline constexpr uint32_t kKnownCapabilities = CapabilityButtons | CapabilityImu | CapabilityAudio;

enum class ParamType : uint8_t {
    Uint32 = 0,
    Color = 1,
    Bool = 2,
    String = 3,
};

enum class Result : uint8_t {
    Ok,
    NullInput,
    PackageTooSmall,
    BadMagic,
    UnsupportedContainerVersion,
    BadHeaderSize,
    UnsupportedFlags,
    BadManifestSize,
    BadWasmSize,
    LengthMismatch,
    DigestVerifierUnavailable,
    DigestMismatch,
    BadManifestCbor,
    UnsupportedManifestVersion,
    BadExtensionId,
    BadDisplayName,
    BadSemanticVersion,
    AbiMismatch,
    FirmwareTooOld,
    GeometryMismatch,
    CapabilityMismatch,
    MemoryLimitExceeded,
    BudgetClassUnsupported,
    BadCompilerMetadata,
    BadSourceDigest,
    BadParamTable,
    BadParamName,
    BadParamType,
    BadParamDefault,
    TooManyStringParams,
    BadWasmHeader,
};

struct SemanticVersion {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
};

struct ParamInfo {
    char name[kMaxParamNameLen + 1];
    ParamType type;
    uint32_t scalarDefault;
    char stringDefault[kMaxStringValueLen + 1];
};

struct Metadata {
    char extensionId[kMaxExtensionIdLen + 1];
    char displayName[kMaxDisplayNameLen + 1];
    SemanticVersion version;
    uint32_t rgbxAbi;
    uint32_t minimumFirmwareAbi;
    uint32_t width;
    uint32_t height;
    uint32_t capabilities;
    uint32_t memoryMaxBytes;
    uint32_t budgetClass;
    char sourceLanguage[kMaxSourceLanguageLen + 1];
    char compilerId[kMaxCompilerIdLen + 1];
    char compilerVersion[kMaxCompilerVersionLen + 1];
    bool hasSourceDigest;
    uint8_t sourceDigest[kDigestSize];
    size_t paramCount;
    size_t stringParamCount;
    ParamInfo params[kMaxParams];
};

struct PackageView {
    Metadata metadata;
    const uint8_t* manifest;
    size_t manifestSize;
    const uint8_t* wasm;
    size_t wasmSize;
};

struct Policy {
    uint32_t rgbxAbi;
    uint32_t firmwareAbi;
    uint32_t width;
    uint32_t height;
    uint32_t allowedCapabilities;
    uint32_t maxMemoryBytes;
    uint32_t maxBudgetClass;
    size_t maxWasmBytes;
};

/**
 * @brief Verifies the package digest trailer.
 *
 * Production uses PSA SHA-256. The callback receives every package byte before
 * the 32-byte trailer and the trailer bytes separately.
 */
using DigestVerifier = bool (*)(void* context, const uint8_t* covered, size_t coveredSize,
                                const uint8_t expected[kDigestSize]);

/**
 * @brief Validate one complete in-memory RGBX package.
 *
 * @param data Complete package bytes.
 * @param size Number of bytes at @p data.
 * @param policy Host ABI, geometry, capability, and resource policy.
 * @param verifyDigest Digest verifier, required even for test packages.
 * @param digestContext Opaque context passed to @p verifyDigest.
 * @param out Replaced only when validation returns Result::Ok.
 * @return Result::Ok or the first failed admission check.
 */
Result validate(const uint8_t* data, size_t size, const Policy& policy, DigestVerifier verifyDigest,
                void* digestContext, PackageView& out);

/** @brief Stable diagnostic string for one admission result. */
const char* describe(Result result);

}  // namespace rgbx_package
