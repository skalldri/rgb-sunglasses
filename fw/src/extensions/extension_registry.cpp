#include <storage/fs_util.h>
#include <extensions/extension_registry.h>

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <array>
#include <cstdio>
#include <cstring>

LOG_MODULE_REGISTER(ext_registry, LOG_LEVEL_INF);

namespace extension_registry {
namespace {
std::array<char[kMaxNameLen], kMaxFiles> sNames = {};
size_t sCount = 0;

constexpr const char kLlextExtension[] = ".llext";
constexpr size_t kLlextExtensionLen = sizeof(kLlextExtension) - 1;

bool hasLlextExtension(const char *fileName) {
    size_t nameLen = strlen(fileName);
    if (nameLen < kLlextExtensionLen) {
        return false;
    }
    return strcmp(fileName + (nameLen - kLlextExtensionLen), kLlextExtension) == 0;
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
        if (hasLlextExtension(name)) {
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

    // Sort by filename (insertion sort — at most kMaxFiles entries) so that
    // slot indices, and therefore extension animation IDs and BLE service
    // UUIDs, are deterministic for a given file set regardless of FAT
    // directory order.
    for (size_t i = 1; i < sCount; i++) {
        char tmp[kMaxNameLen];
        strncpy(tmp, sNames[i], kMaxNameLen - 1);
        tmp[kMaxNameLen - 1] = '\0';
        size_t j = i;
        while (j > 0 && strcmp(sNames[j - 1], tmp) > 0) {
            strncpy(sNames[j], sNames[j - 1], kMaxNameLen - 1);
            sNames[j][kMaxNameLen - 1] = '\0';
            j--;
        }
        strncpy(sNames[j], tmp, kMaxNameLen - 1);
        sNames[j][kMaxNameLen - 1] = '\0';
    }

    LOG_INF("Discovered %zu extension(s) in %s", sCount, kDirectory);
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

}  // namespace extension_registry
