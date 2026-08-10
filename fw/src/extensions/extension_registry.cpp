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
}  // namespace

void init() {
    sCount = 0;

    int rc = fs_mkdir(kDirectory);
    if (rc < 0 && rc != -EEXIST) {
        LOG_ERR("Failed to create %s: %d", kDirectory, rc);
        return;
    }

    // collect_names() sorts as it inserts, so the table is ordered even when the
    // walk aborts partway. That ordering is load-bearing: slot indices become
    // extension animation IDs and BLE service UUIDs (0x40 + slot), so publishing
    // raw FAT order would bind a user's stored extension ID to a different
    // extension's controls, differently on every boot.
    const fs_util::CollectResult found = fs_util::collect_names(kDirectory, kLlextExtension, sNames);
    sCount = found.count;

    if (found.rc < 0) {
        LOG_ERR("Failed to walk %s: %d — serving the %zu extension(s) found before the error",
                kDirectory, found.rc, sCount);
    }
    if (found.skipped > 0) {
        LOG_WRN("%s holds %zu more .llext file(s) than the %zu-slot table; keeping the "
                "alphabetically first %zu — the rest will not appear in `ext list`, the app's "
                "extension list, or as loaded in a FILE_MGMT LIST",
                kDirectory, found.skipped, kMaxFiles, sCount);
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
