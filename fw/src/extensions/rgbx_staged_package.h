#pragma once

#include <extensions/rgbx_package.h>

namespace rgbx_package {

inline constexpr size_t kMaxPackageBytes =
    kHeaderSize + kMaxManifestBytes + kMaxWasmBytes + kDigestSize;

enum class ReadStatus : uint8_t {
    Data,
    End,
    Error,
};

enum class StageResult : uint8_t {
    Ok,
    AlreadyAdmitted,
    ReaderUnavailable,
    ReadFailed,
    ReaderProtocolError,
    PackageTooLarge,
    PackageRejected,
};

struct StageOutcome {
    StageResult result;
    /** True only when packageResult contains the parser's admission result. */
    bool packageChecked;
    Result packageResult;

    bool ok() const {
        return result == StageResult::Ok && packageChecked && packageResult == Result::Ok;
    }
};

/**
 * @brief Sequential byte-source callback for one package snapshot.
 *
 * Data requires 1..capacity bytes, End requires zero bytes, and Error ignores
 * bytesRead. The callback implementation is trusted not to write past capacity.
 */
using ReadNext = ReadStatus (*)(void* context, uint8_t* destination, size_t capacity,
                                size_t& bytesRead);

/**
 * @brief Owns one bounded RGBX package snapshot and its admitted views.
 *
 * The object is intentionally non-copyable because PackageView stores pointers
 * into its byte buffer. Once admitted, load() refuses to overwrite the snapshot
 * until reset() explicitly invalidates it after every consumer has stopped. A
 * package is exposed only after the exact staged bytes pass validation. The
 * owner is not thread-safe; production wiring must serialize its lifecycle.
 */
class StagedPackage final {
   public:
    StagedPackage() = default;
    StagedPackage(const StagedPackage&) = delete;
    StagedPackage& operator=(const StagedPackage&) = delete;
    StagedPackage(StagedPackage&&) = delete;
    StagedPackage& operator=(StagedPackage&&) = delete;

    StageOutcome load(ReadNext readNext, void* readContext, const Policy& policy,
                      DigestVerifier verifyDigest, void* digestContext);

    void reset();
    const PackageView* package() const;
    size_t size() const;

   private:
    uint8_t bytes_[kMaxPackageBytes] = {};
    PackageView view_ = {};
    size_t size_ = 0;
    bool admitted_ = false;
};

const char* describe(StageResult result);

}  // namespace rgbx_package
