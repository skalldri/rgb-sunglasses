#include <extensions/rgbx_staged_package.h>

namespace rgbx_package {
namespace {

StageOutcome streamFailure(StageResult result) {
    return {
        .result = result,
        .packageChecked = false,
        .packageResult = Result::Ok,
    };
}

}  // namespace

StageOutcome StagedPackage::load(ReadNext readNext, void* readContext, const Policy& policy,
                                 DigestVerifier verifyDigest, void* digestContext) {
    if (admitted_) {
        return streamFailure(StageResult::AlreadyAdmitted);
    }
    reset();
    if (readNext == nullptr) {
        return streamFailure(StageResult::ReaderUnavailable);
    }

    size_t stagedSize = 0;
    bool endReached = false;
    while (!endReached && stagedSize < kMaxPackageBytes) {
        const size_t capacity = kMaxPackageBytes - stagedSize;
        size_t bytesRead = 0;
        const ReadStatus status = readNext(readContext, bytes_ + stagedSize, capacity, bytesRead);

        switch (status) {
            case ReadStatus::Data:
                if (bytesRead == 0 || bytesRead > capacity) {
                    return streamFailure(StageResult::ReaderProtocolError);
                }
                stagedSize += bytesRead;
                break;
            case ReadStatus::End:
                if (bytesRead != 0) {
                    return streamFailure(StageResult::ReaderProtocolError);
                }
                endReached = true;
                break;
            case ReadStatus::Error:
                return streamFailure(StageResult::ReadFailed);
            default:
                return streamFailure(StageResult::ReaderProtocolError);
        }
    }

    if (!endReached) {
        uint8_t overflowProbe = 0;
        size_t bytesRead = 0;
        const ReadStatus status = readNext(readContext, &overflowProbe, 1, bytesRead);
        switch (status) {
            case ReadStatus::Data:
                if (bytesRead == 1) {
                    return streamFailure(StageResult::PackageTooLarge);
                }
                return streamFailure(StageResult::ReaderProtocolError);
            case ReadStatus::End:
                if (bytesRead != 0) {
                    return streamFailure(StageResult::ReaderProtocolError);
                }
                break;
            case ReadStatus::Error:
                return streamFailure(StageResult::ReadFailed);
            default:
                return streamFailure(StageResult::ReaderProtocolError);
        }
    }

    PackageView candidate = {};
    const Result packageResult =
        validate(bytes_, stagedSize, policy, verifyDigest, digestContext, candidate);
    if (packageResult != Result::Ok) {
        return {
            .result = StageResult::PackageRejected,
            .packageChecked = true,
            .packageResult = packageResult,
        };
    }

    view_ = candidate;
    size_ = stagedSize;
    admitted_ = true;
    return {
        .result = StageResult::Ok,
        .packageChecked = true,
        .packageResult = Result::Ok,
    };
}

void StagedPackage::reset() {
    view_ = {};
    size_ = 0;
    admitted_ = false;
}

const PackageView* StagedPackage::package() const {
    return admitted_ ? &view_ : nullptr;
}

size_t StagedPackage::size() const {
    return admitted_ ? size_ : 0;
}

const char* describe(StageResult result) {
    switch (result) {
        case StageResult::Ok:
            return "ok";
        case StageResult::AlreadyAdmitted:
            return "package already admitted";
        case StageResult::ReaderUnavailable:
            return "reader unavailable";
        case StageResult::ReadFailed:
            return "read failed";
        case StageResult::ReaderProtocolError:
            return "reader protocol error";
        case StageResult::PackageTooLarge:
            return "package too large";
        case StageResult::PackageRejected:
            return "package rejected";
    }
    return "unknown";
}

}  // namespace rgbx_package
