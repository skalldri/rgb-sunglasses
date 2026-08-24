#include <extensions/rgbx_package.h>
#include <extensions/rgbx_package_psa.h>
#include <extensions/rgbx_staged_package.h>
#include <rgbx/rgbx_v2.h>
#include <psa/crypto.h>
#include <zcbor_common.h>
#include <zcbor_encode.h>
#include <zephyr/ztest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "rgbx_v2_test_package.h"

namespace {

using rgbx_package::CapabilityAudio;
using rgbx_package::CapabilityButtons;
using rgbx_package::CapabilityImu;
using rgbx_package::ParamType;
using rgbx_package::Result;

struct ParamSpec {
    uint32_t type;
    std::string name;
    uint32_t scalarDefault = 0;
    bool boolDefault = false;
    std::string stringDefault;
    bool wrongDefaultType = false;
};

struct ManifestSpec {
    uint32_t manifestVersion = 1;
    std::string extensionId = "demo.pulse";
    std::string displayName = "Demo Pulse";
    uint32_t versionMajor = 1;
    uint32_t versionMinor = 2;
    uint32_t versionPatch = 3;
    uint32_t rgbxAbi = 2;
    uint32_t minimumFirmwareAbi = 1;
    uint32_t width = 40;
    uint32_t height = 12;
    uint32_t capabilities = CapabilityButtons | CapabilityImu;
    uint32_t memoryMaxBytes = 0;
    uint32_t budgetClass = 0;
    std::string sourceLanguage = "rgbx-ast";
    std::string compilerId = "rgbx-phone";
    std::string compilerVersion = "1.0.0";
    std::vector<uint8_t> sourceDigest = std::vector<uint8_t>(32, 0x5a);
    std::vector<ParamSpec> params = {
        {static_cast<uint32_t>(ParamType::Uint32), "Speed", 42},
        {static_cast<uint32_t>(ParamType::Color), "Color", 0x00ff8040},
        {static_cast<uint32_t>(ParamType::Bool), "Enabled", 0, true},
        {static_cast<uint32_t>(ParamType::String), "Label", 0, false, "hello"},
    };
};

rgbx_package::Policy policy() {
    return {
        .rgbxAbi = 2,
        .firmwareAbi = 1,
        .width = 40,
        .height = 12,
        .allowedCapabilities = CapabilityButtons | CapabilityImu | CapabilityAudio,
        .maxMemoryBytes = 0,
        .maxBudgetClass = 0,
        .maxWasmBytes = rgbx_package::kMaxWasmBytes,
    };
}

bool putText(zcbor_state_t* state, std::string_view value) {
    const zcbor_string text = {
        .value = reinterpret_cast<const uint8_t*>(value.data()),
        .len = value.size(),
    };
    return zcbor_tstr_encode(state, &text);
}

bool putBytes(zcbor_state_t* state, const std::vector<uint8_t>& value) {
    const zcbor_string bytes = {.value = value.data(), .len = value.size()};
    return zcbor_bstr_encode(state, &bytes);
}

std::vector<uint8_t> encodeManifest(const ManifestSpec& spec) {
    std::array<uint8_t, rgbx_package::kMaxManifestBytes> buffer{};
    ZCBOR_STATE_E(state, 5, buffer.data(), buffer.size(), 1);

    bool ok =
        zcbor_list_start_encode(state, 15) && zcbor_uint32_put(state, spec.manifestVersion) &&
        putText(state, spec.extensionId) && putText(state, spec.displayName) &&
        zcbor_list_start_encode(state, 3) && zcbor_uint32_put(state, spec.versionMajor) &&
        zcbor_uint32_put(state, spec.versionMinor) && zcbor_uint32_put(state, spec.versionPatch) &&
        zcbor_list_end_encode(state, 3) && zcbor_uint32_put(state, spec.rgbxAbi) &&
        zcbor_uint32_put(state, spec.minimumFirmwareAbi) && zcbor_list_start_encode(state, 2) &&
        zcbor_uint32_put(state, spec.width) && zcbor_uint32_put(state, spec.height) &&
        zcbor_list_end_encode(state, 2) && zcbor_uint32_put(state, spec.capabilities) &&
        zcbor_uint32_put(state, spec.memoryMaxBytes) && zcbor_uint32_put(state, spec.budgetClass) &&
        putText(state, spec.sourceLanguage) && putText(state, spec.compilerId) &&
        putText(state, spec.compilerVersion) && putBytes(state, spec.sourceDigest) &&
        zcbor_list_start_encode(state, spec.params.size());

    for (const ParamSpec& param : spec.params) {
        ok = ok && zcbor_list_start_encode(state, 3) && zcbor_uint32_put(state, param.type) &&
             putText(state, param.name);
        if (param.wrongDefaultType) {
            ok = ok && zcbor_nil_put(state, nullptr);
        } else if (param.type == static_cast<uint32_t>(ParamType::Bool)) {
            ok = ok && zcbor_bool_put(state, param.boolDefault);
        } else if (param.type == static_cast<uint32_t>(ParamType::String)) {
            ok = ok && putText(state, param.stringDefault);
        } else {
            ok = ok && zcbor_uint32_put(state, param.scalarDefault);
        }
        ok = ok && zcbor_list_end_encode(state, 3);
    }

    ok = ok && zcbor_list_end_encode(state, spec.params.size()) && zcbor_list_end_encode(state, 15);
    zassert_true(ok, "manifest fixture encoding failed: %d", zcbor_peek_error(state));
    return {buffer.begin(), buffer.begin() + (state->payload - buffer.data())};
}

std::vector<uint8_t> validWasm() {
    return {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
}

void writeLe16(uint8_t* output, uint16_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32(uint8_t* output, uint32_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8);
    output[2] = static_cast<uint8_t>(value >> 16);
    output[3] = static_cast<uint8_t>(value >> 24);
}

std::array<uint8_t, rgbx_package::kDigestSize> testDigest(const uint8_t* data, size_t size) {
    std::array<uint8_t, rgbx_package::kDigestSize> digest{};
    for (size_t i = 0; i < size; ++i) {
        const size_t lane = i % digest.size();
        digest[lane] = static_cast<uint8_t>((digest[lane] * 33u) ^ data[i] ^ (i & 0xffu));
    }
    return digest;
}

void seal(std::vector<uint8_t>& package) {
    const size_t coveredSize = package.size() - rgbx_package::kDigestSize;
    const auto digest = testDigest(package.data(), coveredSize);
    std::memcpy(package.data() + coveredSize, digest.data(), digest.size());
}

void sealSha256(std::vector<uint8_t>& package) {
    const size_t coveredSize = package.size() - rgbx_package::kDigestSize;
    std::array<uint8_t, rgbx_package::kDigestSize> digest{};
    size_t digestSize = 0;
    const psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, package.data(), coveredSize,
                                                 digest.data(), digest.size(), &digestSize);
    zassert_equal(status, PSA_SUCCESS, "PSA SHA-256 fixture sealing failed: %d", status);
    zassert_equal(digestSize, digest.size());
    std::memcpy(package.data() + coveredSize, digest.data(), digest.size());
}

struct DigestObservation {
    const uint8_t* covered = nullptr;
    size_t coveredSize = 0;
};

enum class ReaderFault {
    None,
    Error,
    ZeroData,
    EndWithData,
    OversizedCount,
    UnknownStatus,
};

struct ReaderFixture {
    const std::vector<uint8_t>* bytes = nullptr;
    size_t offset = 0;
    size_t maxChunk = SIZE_MAX;
    size_t calls = 0;
    size_t faultCall = SIZE_MAX;
    ReaderFault fault = ReaderFault::None;
};

rgbx_package::ReadStatus readFixture(void* context, uint8_t* destination, size_t capacity,
                                     size_t& bytesRead) {
    auto& fixture = *static_cast<ReaderFixture*>(context);
    const size_t call = fixture.calls++;
    if (call == fixture.faultCall) {
        switch (fixture.fault) {
            case ReaderFault::Error:
                return rgbx_package::ReadStatus::Error;
            case ReaderFault::ZeroData:
                bytesRead = 0;
                return rgbx_package::ReadStatus::Data;
            case ReaderFault::EndWithData:
                bytesRead = 1;
                return rgbx_package::ReadStatus::End;
            case ReaderFault::OversizedCount:
                bytesRead = capacity + 1;
                return rgbx_package::ReadStatus::Data;
            case ReaderFault::UnknownStatus:
                return static_cast<rgbx_package::ReadStatus>(0xff);
            case ReaderFault::None:
                break;
        }
    }

    if (fixture.bytes == nullptr || fixture.offset >= fixture.bytes->size()) {
        bytesRead = 0;
        return rgbx_package::ReadStatus::End;
    }

    const size_t remaining = fixture.bytes->size() - fixture.offset;
    size_t chunk = remaining < capacity ? remaining : capacity;
    if (chunk > fixture.maxChunk) {
        chunk = fixture.maxChunk;
    }
    if (chunk == 0) {
        bytesRead = 0;
        return rgbx_package::ReadStatus::Data;
    }

    std::memcpy(destination, fixture.bytes->data() + fixture.offset, chunk);
    fixture.offset += chunk;
    bytesRead = chunk;
    return rgbx_package::ReadStatus::Data;
}

rgbx_package::StagedPackage stagedPackage;

bool verifyTestDigest(void* context, const uint8_t* covered, size_t coveredSize,
                      const uint8_t expected[rgbx_package::kDigestSize]) {
    auto* observation = static_cast<DigestObservation*>(context);
    if (observation != nullptr) {
        observation->covered = covered;
        observation->coveredSize = coveredSize;
    }
    const auto actual = testDigest(covered, coveredSize);
    return std::memcmp(actual.data(), expected, actual.size()) == 0;
}

std::vector<uint8_t> buildPackage(const std::vector<uint8_t>& manifest,
                                  const std::vector<uint8_t>& wasm = validWasm()) {
    std::vector<uint8_t> package(rgbx_package::kHeaderSize + manifest.size() + wasm.size() +
                                 rgbx_package::kDigestSize);
    std::memcpy(package.data(), "RGBX", 4);
    writeLe16(package.data() + 4, rgbx_package::kContainerVersion);
    writeLe16(package.data() + 6, rgbx_package::kHeaderSize);
    writeLe32(package.data() + 8, static_cast<uint32_t>(manifest.size()));
    writeLe32(package.data() + 12, static_cast<uint32_t>(wasm.size()));
    writeLe32(package.data() + 16, 0);
    std::memcpy(package.data() + rgbx_package::kHeaderSize, manifest.data(), manifest.size());
    std::memcpy(package.data() + rgbx_package::kHeaderSize + manifest.size(), wasm.data(),
                wasm.size());
    seal(package);
    return package;
}

Result validate(const std::vector<uint8_t>& package, rgbx_package::PackageView& out,
                const rgbx_package::Policy& activePolicy = policy(),
                DigestObservation* observation = nullptr) {
    return rgbx_package::validate(package.data(), package.size(), activePolicy, verifyTestDigest,
                                  observation, out);
}

Result validateSpec(const ManifestSpec& spec) {
    auto package = buildPackage(encodeManifest(spec));
    rgbx_package::PackageView out{};
    return validate(package, out);
}

// ---------------------------------------------------------------------------
// The package the released SDK actually produces.
//
// Everything above builds its fixtures by hand, which proves the parser but
// not the toolchain: a package builder that encodes a field the device cannot
// read would pass every test above and fail on a user's glasses. The fixtures
// below are the exact bytes fw/sdk/tests/consumer produces through the
// packaged SDK, bound in by fw/sdk/tests/bytes-to-header.mjs and regenerated
// and compared by CI, and they go through this same real parser.
// ---------------------------------------------------------------------------

rgbx_package::Policy releasePolicy() {
    rgbx_package::Policy active = policy();
    active.rgbxAbi = RGBX_V2_ABI_VERSION;
    active.width = RGBX_V2_WIDTH;
    active.height = RGBX_V2_HEIGHT;
    active.allowedCapabilities = RGBX_V2_CAPABILITY_ALL;
    // The memoryless runtime stores a module in a fixed buffer; the container
    // limit is not the profile limit, and it is the smaller one that governs.
    active.maxWasmBytes = RGBX_V2_MODULE_MAX_BYTES;
    return active;
}

std::vector<uint8_t> releasePackage() {
    return {std::begin(kRgbxV2TestPackage), std::end(kRgbxV2TestPackage)};
}

Result validateRelease(const std::vector<uint8_t>& package, rgbx_package::PackageView& out,
                       const rgbx_package::Policy& active) {
    return rgbx_package::validate(package.data(), package.size(), active,
                                  rgbx_package::verifySha256Psa, nullptr, out);
}

Result validateRelease(const std::vector<uint8_t>& package, rgbx_package::PackageView& out) {
    return validateRelease(package, out, releasePolicy());
}

// Sentinel returned by the walker when an item would read past its bound. The
// walker never aborts: a mutation test asserts this value to prove the overrun
// guard fired, rather than crashing the suite.
constexpr size_t kCborOverrun = SIZE_MAX;

struct CborHead {
    uint8_t major;
    uint64_t argument;
    size_t payloadOffset;  // kCborOverrun on overrun
};

// Walk one CBOR item within an explicit bound. It locates byte boundaries and
// decides nothing about admissibility: varying one field inside a REAL package
// needs that field's offset, and re-encoding the manifest here would put a
// test's own encoder back on the path the release parser is meant to judge.
CborHead cborHead(const uint8_t* bytes, size_t size, size_t offset) {
    if (offset >= size) {
        return {0, 0, kCborOverrun};
    }
    const uint8_t initial = bytes[offset++];
    CborHead head{static_cast<uint8_t>(initial >> 5), initial & 0x1fu, offset};
    if (head.argument >= 24) {
        if (head.argument > 27) {
            return {0, 0, kCborOverrun};
        }
        const size_t width = size_t{1} << (head.argument - 24);
        if (size - offset < width) {
            return {0, 0, kCborOverrun};
        }
        head.argument = 0;
        for (size_t i = 0; i < width; ++i) {
            head.argument = (head.argument << 8) | bytes[offset + i];
        }
        head.payloadOffset = offset + width;
    }
    return head;
}

size_t cborItemEnd(const uint8_t* bytes, size_t size, size_t offset) {
    const CborHead head = cborHead(bytes, size, offset);
    if (head.payloadOffset == kCborOverrun) {
        return kCborOverrun;
    }
    switch (head.major) {
        case 0:  // unsigned
        case 1:  // negative
        case 7:  // simple values, including the booleans this manifest uses
            return head.payloadOffset;
        case 2:  // byte string
        case 3:  // text string
            if (size - head.payloadOffset < head.argument) {
                return kCborOverrun;
            }
            return head.payloadOffset + static_cast<size_t>(head.argument);
        case 4: {  // array
            size_t end = head.payloadOffset;
            for (uint64_t i = 0; i < head.argument; ++i) {
                end = cborItemEnd(bytes, size, end);
                if (end == kCborOverrun) {
                    return kCborOverrun;
                }
            }
            return end;
        }
        default:
            return kCborOverrun;
    }
}

size_t packageManifestSize(const std::vector<uint8_t>& package) {
    return static_cast<size_t>(package[8]) | (static_cast<size_t>(package[9]) << 8) |
           (static_cast<size_t>(package[10]) << 16) | (static_cast<size_t>(package[11]) << 24);
}

size_t packageWasmSize(const std::vector<uint8_t>& package) {
    return static_cast<size_t>(package[12]) | (static_cast<size_t>(package[13]) << 8) |
           (static_cast<size_t>(package[14]) << 16) | (static_cast<size_t>(package[15]) << 24);
}

// Byte offset of manifest element `index`, relative to the start of the
// package, or kCborOverrun if the walk leaves the manifest. The bound is the
// DECLARED manifest length: bounding by manifest+wasm (the whole payload) lets
// a walk past the array wander into the module bytes without the guard firing.
size_t manifestFieldOffset(const std::vector<uint8_t>& package, size_t index) {
    const uint8_t* manifest = package.data() + rgbx_package::kHeaderSize;
    const size_t manifestSize = packageManifestSize(package);
    const CborHead head = cborHead(manifest, manifestSize, 0);
    if (head.payloadOffset == kCborOverrun) {
        return kCborOverrun;
    }
    size_t offset = head.payloadOffset;
    for (size_t i = 0; i < index; ++i) {
        offset = cborItemEnd(manifest, manifestSize, offset);
        if (offset == kCborOverrun) {
            return kCborOverrun;
        }
    }
    return rgbx_package::kHeaderSize + offset;
}

// Index of each manifest element, in the canonical order the SDK emits.
constexpr size_t kFieldMemoryMaxBytes = 8;
constexpr size_t kFieldBudgetClass = 9;

void hexToBytes(const char* hex, uint8_t* out, size_t outLen) {
    zassert_equal(std::strlen(hex), outLen * 2, "hex digest has the wrong length");
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < outLen; ++i) {
        const int hi = nibble(hex[i * 2]);
        const int lo = nibble(hex[i * 2 + 1]);
        zassert_true(hi >= 0 && lo >= 0, "non-hex character in digest");
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
}

void* setupSuite() {
    const psa_status_t status = psa_crypto_init();
    zassert_equal(status, PSA_SUCCESS, "PSA Crypto initialization failed: %d", status);
    return nullptr;
}

}  // namespace

ZTEST_SUITE(rgbx_package, nullptr, setupSuite, nullptr, nullptr, nullptr);

ZTEST(rgbx_package, test_psa_sha256_verifier_matches_standard_vector) {
    constexpr std::array<uint8_t, 3> kMessage = {'a', 'b', 'c'};
    constexpr std::array<uint8_t, rgbx_package::kDigestSize> kExpected = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };

    zassert_true(
        rgbx_package::verifySha256Psa(nullptr, kMessage.data(), kMessage.size(), kExpected.data()));

    auto wrong = kExpected;
    wrong[0] ^= 0x01;
    zassert_false(
        rgbx_package::verifySha256Psa(nullptr, kMessage.data(), kMessage.size(), wrong.data()));
    zassert_false(
        rgbx_package::verifySha256Psa(nullptr, nullptr, kMessage.size(), kExpected.data()));
    zassert_false(
        rgbx_package::verifySha256Psa(nullptr, kMessage.data(), kMessage.size(), nullptr));
}

ZTEST(rgbx_package, test_psa_sha256_admits_package_and_rejects_every_tampered_region) {
    const auto manifest = encodeManifest(ManifestSpec{});
    auto package = buildPackage(manifest);
    sealSha256(package);
    rgbx_package::PackageView out{};

    zassert_equal(rgbx_package::validate(package.data(), package.size(), policy(),
                                         rgbx_package::verifySha256Psa, nullptr, out),
                  Result::Ok);

    auto tamperedHeader = package;
    writeLe32(tamperedHeader.data() + 8, static_cast<uint32_t>(manifest.size() - 1));
    writeLe32(tamperedHeader.data() + 12, static_cast<uint32_t>(validWasm().size() + 1));
    zassert_equal(rgbx_package::validate(tamperedHeader.data(), tamperedHeader.size(), policy(),
                                         rgbx_package::verifySha256Psa, nullptr, out),
                  Result::DigestMismatch);

    const std::array<size_t, 3> coveredOffsets = {
        rgbx_package::kHeaderSize,
        package.size() - rgbx_package::kDigestSize - 1,
        package.size() - 1,
    };
    for (const size_t offset : coveredOffsets) {
        auto tampered = package;
        tampered[offset] ^= 0x01;
        zassert_equal(rgbx_package::validate(tampered.data(), tampered.size(), policy(),
                                             rgbx_package::verifySha256Psa, nullptr, out),
                      Result::DigestMismatch);
    }
}

ZTEST(rgbx_package, test_staging_owns_one_chunked_validated_snapshot) {
    stagedPackage.reset();
    const auto manifest = encodeManifest(ManifestSpec{});
    auto package = buildPackage(manifest);
    sealSha256(package);
    ReaderFixture reader = {
        .bytes = &package,
        .maxChunk = 7,
    };

    const auto outcome =
        stagedPackage.load(readFixture, &reader, policy(), rgbx_package::verifySha256Psa, nullptr);
    zassert_true(outcome.ok());
    zassert_true(outcome.packageChecked);
    zassert_equal(outcome.packageResult, Result::Ok);
    zassert_true(reader.calls > 2);
    zassert_equal(stagedPackage.size(), package.size());
    zassert_not_null(stagedPackage.package());
    zassert_str_equal(stagedPackage.package()->metadata.extensionId, "demo.pulse");
    zassert_not_equal(stagedPackage.package()->wasm,
                      package.data() + rgbx_package::kHeaderSize + manifest.size());
}

ZTEST(rgbx_package, test_staging_rejects_truncation_and_digest_mismatch_with_parser_reason) {
    stagedPackage.reset();
    auto package = buildPackage(encodeManifest(ManifestSpec{}));
    sealSha256(package);

    auto truncated = package;
    truncated.pop_back();
    ReaderFixture reader = {.bytes = &truncated};
    auto outcome =
        stagedPackage.load(readFixture, &reader, policy(), rgbx_package::verifySha256Psa, nullptr);
    zassert_equal(outcome.result, rgbx_package::StageResult::PackageRejected);
    zassert_true(outcome.packageChecked);
    zassert_equal(outcome.packageResult, Result::LengthMismatch);
    zassert_is_null(stagedPackage.package());

    package[rgbx_package::kHeaderSize] ^= 0x01;
    reader = {.bytes = &package};
    outcome =
        stagedPackage.load(readFixture, &reader, policy(), rgbx_package::verifySha256Psa, nullptr);
    zassert_equal(outcome.result, rgbx_package::StageResult::PackageRejected);
    zassert_true(outcome.packageChecked);
    zassert_equal(outcome.packageResult, Result::DigestMismatch);
    zassert_is_null(stagedPackage.package());
}

ZTEST(rgbx_package, test_staging_distinguishes_exact_capacity_from_one_byte_oversize) {
    stagedPackage.reset();
    std::vector<uint8_t> exact(rgbx_package::kMaxPackageBytes, 0);
    ReaderFixture reader = {.bytes = &exact};
    auto outcome =
        stagedPackage.load(readFixture, &reader, policy(), rgbx_package::verifySha256Psa, nullptr);
    zassert_equal(outcome.result, rgbx_package::StageResult::PackageRejected);
    zassert_true(outcome.packageChecked);
    zassert_equal(outcome.packageResult, Result::BadMagic);

    exact.push_back(0);
    reader = {.bytes = &exact};
    outcome =
        stagedPackage.load(readFixture, &reader, policy(), rgbx_package::verifySha256Psa, nullptr);
    zassert_equal(outcome.result, rgbx_package::StageResult::PackageTooLarge);
    zassert_false(outcome.packageChecked);
    zassert_is_null(stagedPackage.package());
    zassert_equal(stagedPackage.size(), 0);
}

ZTEST(rgbx_package, test_staging_fails_closed_on_reader_errors_and_protocol_violations) {
    stagedPackage.reset();
    auto package = buildPackage(encodeManifest(ManifestSpec{}));
    sealSha256(package);

    ReaderFixture validReader = {.bytes = &package};
    zassert_true(
        stagedPackage
            .load(readFixture, &validReader, policy(), rgbx_package::verifySha256Psa, nullptr)
            .ok());
    zassert_not_null(stagedPackage.package());

    ReaderFixture blockedReader = {.bytes = &package};
    auto outcome = stagedPackage.load(readFixture, &blockedReader, policy(),
                                      rgbx_package::verifySha256Psa, nullptr);
    zassert_equal(outcome.result, rgbx_package::StageResult::AlreadyAdmitted);
    zassert_equal(blockedReader.calls, 0);
    zassert_not_null(stagedPackage.package());

    stagedPackage.reset();
    outcome =
        stagedPackage.load(nullptr, nullptr, policy(), rgbx_package::verifySha256Psa, nullptr);
    zassert_equal(outcome.result, rgbx_package::StageResult::ReaderUnavailable);
    zassert_is_null(stagedPackage.package());

    constexpr std::array<ReaderFault, 4> kProtocolFaults = {
        ReaderFault::ZeroData,
        ReaderFault::EndWithData,
        ReaderFault::OversizedCount,
        ReaderFault::UnknownStatus,
    };
    for (const ReaderFault fault : kProtocolFaults) {
        ReaderFixture reader = {
            .bytes = &package,
            .faultCall = 0,
            .fault = fault,
        };
        outcome = stagedPackage.load(readFixture, &reader, policy(), rgbx_package::verifySha256Psa,
                                     nullptr);
        zassert_equal(outcome.result, rgbx_package::StageResult::ReaderProtocolError);
        zassert_false(outcome.packageChecked);
        zassert_is_null(stagedPackage.package());
    }

    ReaderFixture reader = {
        .bytes = &package,
        .maxChunk = 8,
        .faultCall = 2,
        .fault = ReaderFault::Error,
    };
    outcome =
        stagedPackage.load(readFixture, &reader, policy(), rgbx_package::verifySha256Psa, nullptr);
    zassert_equal(outcome.result, rgbx_package::StageResult::ReadFailed);
    zassert_false(outcome.packageChecked);
    zassert_is_null(stagedPackage.package());
}

ZTEST(rgbx_package, test_staging_reset_invalidates_an_admitted_view) {
    stagedPackage.reset();
    auto package = buildPackage(encodeManifest(ManifestSpec{}));
    sealSha256(package);
    ReaderFixture reader = {.bytes = &package};
    zassert_true(
        stagedPackage.load(readFixture, &reader, policy(), rgbx_package::verifySha256Psa, nullptr)
            .ok());

    stagedPackage.reset();
    zassert_is_null(stagedPackage.package());
    zassert_equal(stagedPackage.size(), 0);
}

ZTEST(rgbx_package, test_valid_golden_package_copies_metadata_and_views) {
    const auto manifest = encodeManifest(ManifestSpec{});
    auto package = buildPackage(manifest);
    rgbx_package::PackageView out{};
    DigestObservation observation;

    zassert_equal(validate(package, out, policy(), &observation), Result::Ok);
    zassert_equal(observation.covered, package.data());
    zassert_equal(observation.coveredSize, package.size() - rgbx_package::kDigestSize);
    zassert_mem_equal(out.manifest, manifest.data(), manifest.size());
    zassert_equal(out.manifestSize, manifest.size());
    const auto wasm = validWasm();
    zassert_mem_equal(out.wasm, wasm.data(), wasm.size());
    zassert_equal(out.wasmSize, wasm.size());
    zassert_str_equal(out.metadata.extensionId, "demo.pulse");
    zassert_str_equal(out.metadata.displayName, "Demo Pulse");
    zassert_equal(out.metadata.version.major, 1);
    zassert_equal(out.metadata.version.minor, 2);
    zassert_equal(out.metadata.version.patch, 3);
    zassert_equal(out.metadata.paramCount, 4);
    zassert_equal(out.metadata.stringParamCount, 1);
    zassert_equal(out.metadata.params[1].scalarDefault, 0x00ff8040);
    zassert_equal(out.metadata.params[2].scalarDefault, 1);
    zassert_str_equal(out.metadata.params[3].stringDefault, "hello");
    zassert_true(out.metadata.hasSourceDigest);
    zassert_equal(out.metadata.sourceDigest[0], 0x5a);
}

ZTEST(rgbx_package, test_every_truncated_boundary_fails_and_output_is_unchanged) {
    auto package = buildPackage(encodeManifest(ManifestSpec{}));
    for (size_t size = 0; size < package.size(); ++size) {
        rgbx_package::PackageView out{};
        constexpr char kSentinel[] = "sentinel";
        std::memcpy(out.metadata.extensionId, kSentinel, sizeof(kSentinel));
        out.wasm = reinterpret_cast<const uint8_t*>(0x1234);
        const Result result =
            rgbx_package::validate(package.data(), size, policy(), verifyTestDigest, nullptr, out);
        zassert_not_equal(result, Result::Ok, "prefix %zu was accepted", size);
        zassert_str_equal(out.metadata.extensionId, "sentinel");
        zassert_equal(out.wasm, reinterpret_cast<const uint8_t*>(0x1234));
    }
}

ZTEST(rgbx_package, test_envelope_fields_and_exact_length_fail_closed) {
    auto base = buildPackage(encodeManifest(ManifestSpec{}));
    rgbx_package::PackageView out{};

    zassert_equal(
        rgbx_package::validate(nullptr, base.size(), policy(), verifyTestDigest, nullptr, out),
        Result::NullInput);

    auto package = base;
    package[0] = 'X';
    seal(package);
    zassert_equal(validate(package, out), Result::BadMagic);

    package = base;
    writeLe16(package.data() + 4, 2);
    seal(package);
    zassert_equal(validate(package, out), Result::UnsupportedContainerVersion);

    package = base;
    writeLe16(package.data() + 6, 24);
    seal(package);
    zassert_equal(validate(package, out), Result::BadHeaderSize);

    package = base;
    writeLe32(package.data() + 16, 1);
    seal(package);
    zassert_equal(validate(package, out), Result::UnsupportedFlags);

    package = base;
    writeLe32(package.data() + 8, 0);
    seal(package);
    zassert_equal(validate(package, out), Result::BadManifestSize);

    package = base;
    writeLe32(package.data() + 8, rgbx_package::kMaxManifestBytes + 1);
    seal(package);
    zassert_equal(validate(package, out), Result::BadManifestSize);

    package = base;
    package.insert(package.end() - rgbx_package::kDigestSize, 0);
    seal(package);
    zassert_equal(validate(package, out), Result::LengthMismatch);
}

ZTEST(rgbx_package, test_digest_is_required_and_covers_header_manifest_and_wasm) {
    auto package = buildPackage(encodeManifest(ManifestSpec{}));
    rgbx_package::PackageView out{};
    zassert_equal(
        rgbx_package::validate(package.data(), package.size(), policy(), nullptr, nullptr, out),
        Result::DigestVerifierUnavailable);

    const std::array<size_t, 2> coveredOffsets = {
        rgbx_package::kHeaderSize,
        package.size() - rgbx_package::kDigestSize - 1,
    };
    for (const size_t offset : coveredOffsets) {
        auto corrupted = package;
        corrupted[offset] ^= 0x01;
        zassert_equal(validate(corrupted, out), Result::DigestMismatch);
    }

    package.back() ^= 0x01;
    zassert_equal(validate(package, out), Result::DigestMismatch);
}

ZTEST(rgbx_package, test_noncanonical_missing_extra_and_trailing_manifest_data_are_rejected) {
    const auto canonical = encodeManifest(ManifestSpec{});
    zassert_equal(canonical[0], 0x8f, "fixture must start with a 15-item canonical array");
    zassert_equal(canonical[1], 0x01, "fixture must encode schema version minimally");
    rgbx_package::PackageView out{};

    auto nonminimal = canonical;
    nonminimal[1] = 0x18;
    nonminimal.insert(nonminimal.begin() + 2, 0x01);
    auto package = buildPackage(nonminimal);
    zassert_equal(validate(package, out), Result::BadManifestCbor);

    auto indefinite = canonical;
    indefinite[0] = 0x9f;
    indefinite.push_back(0xff);
    package = buildPackage(indefinite);
    zassert_equal(validate(package, out), Result::BadManifestCbor);

    auto extra = canonical;
    extra[0] = 0x90;
    extra.push_back(0x00);
    package = buildPackage(extra);
    zassert_equal(validate(package, out), Result::BadManifestCbor);

    auto missing = canonical;
    missing[0] = 0x8e;
    package = buildPackage(missing);
    zassert_not_equal(validate(package, out), Result::Ok);

    auto trailing = canonical;
    trailing.push_back(0x00);
    package = buildPackage(trailing);
    zassert_equal(validate(package, out), Result::BadManifestCbor);
}

ZTEST(rgbx_package, test_identity_and_compiler_metadata_are_bounded_ascii) {
    ManifestSpec spec;
    spec.extensionId = "Bad.Id";
    zassert_equal(validateSpec(spec), Result::BadExtensionId);

    spec = {};
    spec.extensionId = ".leading";
    zassert_equal(validateSpec(spec), Result::BadExtensionId);

    spec = {};
    spec.displayName = std::string("bad") + static_cast<char>(0x80);
    zassert_equal(validateSpec(spec), Result::BadDisplayName);

    spec = {};
    spec.compilerId.clear();
    zassert_equal(validateSpec(spec), Result::BadCompilerMetadata);

    spec = {};
    spec.sourceDigest.resize(31);
    zassert_equal(validateSpec(spec), Result::BadSourceDigest);
}

ZTEST(rgbx_package, test_abi_geometry_capability_memory_and_budget_policy) {
    ManifestSpec spec;
    spec.manifestVersion = 2;
    zassert_equal(validateSpec(spec), Result::UnsupportedManifestVersion);

    spec = {};
    spec.versionMajor = 70000;
    zassert_equal(validateSpec(spec), Result::BadSemanticVersion);

    spec = {};
    spec.rgbxAbi = 3;
    zassert_equal(validateSpec(spec), Result::AbiMismatch);

    spec = {};
    spec.minimumFirmwareAbi = 2;
    zassert_equal(validateSpec(spec), Result::FirmwareTooOld);

    spec = {};
    spec.width = 41;
    zassert_equal(validateSpec(spec), Result::GeometryMismatch);

    spec = {};
    spec.capabilities = 1u << 31;
    zassert_equal(validateSpec(spec), Result::CapabilityMismatch);

    spec = {};
    spec.memoryMaxBytes = 1;
    zassert_equal(validateSpec(spec), Result::MemoryLimitExceeded);

    spec = {};
    spec.budgetClass = 1;
    zassert_equal(validateSpec(spec), Result::BudgetClassUnsupported);
}

ZTEST(rgbx_package, test_parameter_schema_rejects_ambiguity_and_bad_defaults) {
    ManifestSpec spec;
    spec.params.clear();
    for (size_t i = 0; i < rgbx_package::kMaxParams + 1; ++i) {
        spec.params.push_back(
            {static_cast<uint32_t>(ParamType::Uint32), "P" + std::to_string(i), 1});
    }
    zassert_equal(validateSpec(spec), Result::BadParamTable);

    spec = {};
    spec.params[1].name = spec.params[0].name;
    zassert_equal(validateSpec(spec), Result::BadParamName);

    spec = {};
    spec.params[0].type = 99;
    zassert_equal(validateSpec(spec), Result::BadParamType);

    spec = {};
    spec.params[0].wrongDefaultType = true;
    zassert_equal(validateSpec(spec), Result::BadParamDefault);

    spec = {};
    spec.params[1].scalarDefault = 0xff000000;
    zassert_equal(validateSpec(spec), Result::BadParamDefault);

    spec = {};
    spec.params.clear();
    for (size_t i = 0; i < rgbx_package::kMaxStringParams + 1; ++i) {
        spec.params.push_back(
            {static_cast<uint32_t>(ParamType::String), "Text" + std::to_string(i), 0, false, "x"});
    }
    zassert_equal(validateSpec(spec), Result::TooManyStringParams);

    spec = {};
    spec.params[3].stringDefault.assign(rgbx_package::kMaxStringValueLen + 1, 'x');
    zassert_equal(validateSpec(spec), Result::BadParamDefault);
}

ZTEST(rgbx_package, test_wasm_header_and_size_policy_are_checked_last) {
    const auto manifest = encodeManifest(ManifestSpec{});
    rgbx_package::PackageView out{};

    auto wasm = validWasm();
    wasm[0] = 1;
    auto package = buildPackage(manifest, wasm);
    zassert_equal(validate(package, out), Result::BadWasmHeader);

    wasm = validWasm();
    wasm[4] = 2;
    package = buildPackage(manifest, wasm);
    zassert_equal(validate(package, out), Result::BadWasmHeader);

    wasm.resize(7);
    package = buildPackage(manifest, wasm);
    zassert_equal(validate(package, out), Result::BadWasmSize);

    auto limitedPolicy = policy();
    limitedPolicy.maxWasmBytes = 7;
    package = buildPackage(manifest);
    zassert_equal(validate(package, out, limitedPolicy), Result::BadWasmSize);
}

ZTEST(rgbx_package, test_describe_covers_every_result) {
    for (uint32_t raw = static_cast<uint32_t>(Result::Ok);
         raw <= static_cast<uint32_t>(Result::BadWasmHeader); ++raw) {
        zassert_not_equal(std::strcmp(rgbx_package::describe(static_cast<Result>(raw)), "unknown"),
                          0, "missing description for result %u", raw);
    }

    for (uint32_t raw = static_cast<uint32_t>(rgbx_package::StageResult::Ok);
         raw <= static_cast<uint32_t>(rgbx_package::StageResult::PackageRejected); ++raw) {
        zassert_not_equal(
            std::strcmp(rgbx_package::describe(static_cast<rgbx_package::StageResult>(raw)),
                        "unknown"),
            0, "missing staging description for result %u", raw);
    }
}

ZTEST(rgbx_package, test_release_sdk_package_is_admitted_by_the_real_parser) {
    const auto package = releasePackage();
    rgbx_package::PackageView out{};
    const Result admitted = validateRelease(package, out);
    zassert_equal(admitted, Result::Ok, "the released SDK package was refused: %s",
                  rgbx_package::describe(admitted));

    zassert_equal(std::strcmp(out.metadata.extensionId, "rgbx_v2_test"), 0);
    zassert_equal(std::strcmp(out.metadata.displayName, "RGBX v2 SDK Test"), 0);
    zassert_equal(out.metadata.version.major, 0);
    zassert_equal(out.metadata.version.minor, 0);
    zassert_equal(out.metadata.version.patch, 0);
    zassert_equal(out.metadata.rgbxAbi, RGBX_V2_ABI_VERSION);
    zassert_equal(out.metadata.minimumFirmwareAbi, 1);
    zassert_equal(out.metadata.width, RGBX_V2_WIDTH);
    zassert_equal(out.metadata.height, RGBX_V2_HEIGHT);
    zassert_equal(out.metadata.capabilities, CapabilityButtons);
    zassert_equal(out.metadata.memoryMaxBytes, 0);
    zassert_equal(out.metadata.budgetClass, 0);
    zassert_equal(std::strcmp(out.metadata.sourceLanguage, "c"), 0);
    zassert_equal(std::strcmp(out.metadata.compilerId, "wasi-sdk"), 0);
    // The compiler pin the SDK manifest carries; a package built by a
    // different wasi-sdk would not have been sealed in the first place.
    zassert_equal(std::strcmp(out.metadata.compilerVersion, "33.0"), 0);
    zassert_true(out.metadata.hasSourceDigest);
    zassert_equal(out.metadata.paramCount, 1);
    zassert_equal(out.metadata.stringParamCount, 0);
    zassert_equal(std::strcmp(out.metadata.params[0].name, "Color"), 0);
    zassert_equal(out.metadata.params[0].type, ParamType::Color);
    zassert_equal(out.metadata.params[0].scalarDefault, 0x00ff00ffu);

    // The recorded source digest must be the SHA-256 of the exact source the
    // SDK consumer compiled, supplied at build time by CMake's file(SHA256)
    // over that same rgbx_v2_test.c.
    uint8_t expectedSourceDigest[rgbx_package::kDigestSize];
    hexToBytes(RGBX_V2_TEST_SOURCE_SHA256, expectedSourceDigest, sizeof(expectedSourceDigest));
    zassert_mem_equal(out.metadata.sourceDigest, expectedSourceDigest,
                      rgbx_package::kDigestSize,
                      "source digest is not the SHA-256 of the compiled source");

    // Prove the parser returns the COMPLETE module span, not just a valid
    // magic. Slice the module region straight out of the package by its
    // declared offsets and compare every byte.
    const size_t manifestLen = packageManifestSize(package);
    const size_t wasmLen = packageWasmSize(package);
    zassert_equal(out.wasmSize, wasmLen, "parser did not return the whole module payload");
    zassert_mem_equal(out.wasm, package.data() + rgbx_package::kHeaderSize + manifestLen, wasmLen,
                      "returned module bytes are not the packaged module");
    static const uint8_t kWasmMagic[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    zassert_mem_equal(out.wasm, kWasmMagic, sizeof(kWasmMagic));
}

ZTEST(rgbx_package, test_manifest_field_walker_guard_fires_past_the_manifest) {
    const auto package = releasePackage();
    // A valid in-range field resolves to a real offset.
    zassert_not_equal(manifestFieldOffset(package, kFieldBudgetClass), kCborOverrun);
    // The manifest's top-level array holds 15 elements (indices 0..14).
    // Element 15 lands exactly at the manifest's end; asking for element 16
    // must trip the overrun guard rather than reading the module bytes that
    // follow. It only trips because the bound is the DECLARED manifest length:
    // bounding by manifest+wasm would let the walk parse the module bytes as
    // CBOR and return a bogus in-payload offset, and this assertion would fail.
    zassert_equal(manifestFieldOffset(package, 16), kCborOverrun,
                  "walking one element past the manifest array must be caught by the bound");
}

ZTEST(rgbx_package, test_release_sdk_package_envelope_negatives) {
    const auto original = releasePackage();

    struct EnvelopeCase {
        const char* name;
        size_t offset;
        uint8_t value;
        Result expected;
    };
    const EnvelopeCase cases[] = {
        {"magic", 0, 'r', Result::BadMagic},
        {"container version", 4, 2, Result::UnsupportedContainerVersion},
        {"header size", 6, 24, Result::BadHeaderSize},
        {"reserved flags", 16, 1, Result::UnsupportedFlags},
        {"empty manifest", 8, 0, Result::BadManifestSize},
        {"empty module", 12, 0, Result::BadWasmSize},
    };
    for (const EnvelopeCase& envelope : cases) {
        auto package = original;
        package[envelope.offset] = envelope.value;
        rgbx_package::PackageView out{};
        zassert_equal(validateRelease(package, out), envelope.expected,
                      "envelope case '%s' was not refused as expected", envelope.name);
    }

    auto shortened = original;
    shortened.pop_back();
    rgbx_package::PackageView out{};
    zassert_equal(validateRelease(shortened, out), Result::LengthMismatch);

    auto shifted = original;
    ++shifted[8];
    zassert_equal(validateRelease(shifted, out), Result::LengthMismatch);

    // Every covered region and the trailer itself.
    const size_t tampered[] = {
        rgbx_package::kHeaderSize,
        original.size() - rgbx_package::kDigestSize - 1,
        original.size() - 1,
    };
    for (const size_t offset : tampered) {
        auto package = original;
        package[offset] ^= 0x01;
        zassert_equal(validateRelease(package, out), Result::DigestMismatch,
                      "tampering at offset %zu was not caught by the digest", offset);
    }
}

ZTEST(rgbx_package, test_release_sdk_package_is_refused_by_every_policy_dimension) {
    const auto package = releasePackage();
    rgbx_package::PackageView out{};

    auto withPolicy = [&](auto mutate, Result expected, const char* label) {
        rgbx_package::Policy active = releasePolicy();
        mutate(active);
        zassert_equal(validateRelease(package, out, active), expected,
                      "policy dimension '%s' did not refuse the release package", label);
    };

    withPolicy([](rgbx_package::Policy& p) { p.rgbxAbi = RGBX_V2_ABI_VERSION + 1; },
               Result::AbiMismatch, "rgbxAbi");
    withPolicy([](rgbx_package::Policy& p) { p.firmwareAbi = 0; },
               Result::FirmwareTooOld, "firmwareAbi");
    withPolicy([](rgbx_package::Policy& p) { p.width = RGBX_V2_WIDTH + 1; },
               Result::GeometryMismatch, "width");
    withPolicy([](rgbx_package::Policy& p) { p.height = RGBX_V2_HEIGHT + 1; },
               Result::GeometryMismatch, "height");
    withPolicy([](rgbx_package::Policy& p) { p.allowedCapabilities = CapabilityImu; },
               Result::CapabilityMismatch, "allowedCapabilities");
    withPolicy([&](rgbx_package::Policy& p) { p.maxWasmBytes = packageWasmSize(package) - 1; },
               Result::BadWasmSize, "maxWasmBytes");
}

ZTEST(rgbx_package, test_release_sdk_package_manifest_limits_are_enforced) {
    const auto original = releasePackage();

    // Both fields are a zero the packager refuses to emit as anything else, so
    // the only way to reach these two policy dimensions with real release
    // bytes is to alter the sealed manifest and re-seal it.
    struct FieldCase {
        const char* name;
        size_t index;
        Result expected;
    };
    const FieldCase cases[] = {
        {"memoryMaxBytes", kFieldMemoryMaxBytes, Result::MemoryLimitExceeded},
        {"budgetClass", kFieldBudgetClass, Result::BudgetClassUnsupported},
    };
    for (const FieldCase& field : cases) {
        auto package = original;
        const size_t offset = manifestFieldOffset(package, field.index);
        zassert_not_equal(offset, kCborOverrun, "%s offset walked off the manifest", field.name);
        zassert_equal(package[offset], 0, "%s was expected to be a single zero byte", field.name);
        package[offset] = 1;
        sealSha256(package);
        rgbx_package::PackageView out{};
        zassert_equal(validateRelease(package, out), field.expected,
                      "manifest field '%s' was not refused as expected", field.name);
    }
}
