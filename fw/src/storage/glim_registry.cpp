#include <storage/fs_util.h>
#include <storage/glim_registry.h>

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <array>
#include <cstdio>
#include <cstring>

LOG_MODULE_REGISTER(glim_registry, LOG_LEVEL_INF);

namespace glim_registry {
namespace {
std::array<char[kMaxNameLen], kMaxFiles> sNames = {};
size_t sCount = 0;

constexpr const char *kGlimExtension = ".glim";
}  // namespace

void init() {
    sCount = 0;

    int rc = fs_mkdir(kDirectory);
    if (rc < 0 && rc != -EEXIST) {
        LOG_ERR("Failed to create %s: %d", kDirectory, rc);
        return;
    }

    const fs_util::CollectResult found = fs_util::collect_names(kDirectory, kGlimExtension, sNames);
    sCount = found.count;

    // Report the count on the error path too, not just the happy one. Partial
    // success and total failure look identical from "Failed to walk ...: -5" alone,
    // and the realistic report is "most of my animations disappeared" — an operator
    // reading the boot log should be able to tell "3 of 12 registered before the
    // volume errored" without reproducing the walk.
    if (found.rc < 0) {
        LOG_ERR("Failed to walk %s: %d — serving the %zu file(s) found before the error",
                kDirectory, found.rc, sCount);
    }
    if (found.skipped > 0) {
        LOG_WRN("%s holds %zu more .glim file(s) than the %zu-entry table; keeping the "
                "alphabetically first %zu — the rest will not appear in `glim list`",
                kDirectory, found.skipped, kMaxFiles, sCount);
    }

    LOG_INF("Discovered %zu file(s) in %s", sCount, kDirectory);
}

size_t count() {
    return sCount;
}

const char *name(size_t index) {
    if (index >= sCount) {
        return nullptr;
    }
    return sNames[index];
}

bool full_path(size_t index, char *out, size_t outLen) {
    const char *fileName = name(index);
    if (!fileName) {
        return false;
    }

    int written = snprintf(out, outLen, "%s/%s", kDirectory, fileName);
    return written > 0 && static_cast<size_t>(written) < outLen;
}

}  // namespace glim_registry
