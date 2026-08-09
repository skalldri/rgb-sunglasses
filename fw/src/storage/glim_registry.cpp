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

bool hasGlimExtension(const char *fileName) {
    size_t nameLen = strlen(fileName);
    size_t extLen = strlen(kGlimExtension);
    if (nameLen < extLen) {
        return false;
    }
    return strcmp(fileName + (nameLen - extLen), kGlimExtension) == 0;
}
}  // namespace

void init() {
    sCount = 0;

    int rc = fs_mkdir(kDirectory);
    if (rc < 0 && rc != -EEXIST) {
        LOG_ERR("Failed to create %s: %d", kDirectory, rc);
        return;
    }

    // Stops at kMaxFiles: the table is fixed-size, and silently overrunning it
    // would be worse than ignoring the tail of an over-full directory.
    rc = fs_util::for_each_file(kDirectory, [](const char *name) {
        if (hasGlimExtension(name)) {
            strncpy(sNames[sCount], name, kMaxNameLen - 1);
            sNames[sCount][kMaxNameLen - 1] = '\0';
            sCount++;
        }
        return sCount < kMaxFiles;
    });
    if (rc < 0) {
        LOG_ERR("Failed to walk %s: %d", kDirectory, rc);
        return;
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
