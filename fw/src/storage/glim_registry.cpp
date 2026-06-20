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
}  // namespace

void init() {
    sCount = 0;

    int rc = fs_mkdir(kDirectory);
    if (rc < 0 && rc != -EEXIST) {
        LOG_ERR("Failed to create %s: %d", kDirectory, rc);
        return;
    }

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);

    rc = fs_opendir(&dir, kDirectory);
    if (rc < 0) {
        LOG_ERR("Failed to open %s: %d", kDirectory, rc);
        return;
    }

    while (sCount < kMaxFiles) {
        struct fs_dirent entry;
        rc = fs_readdir(&dir, &entry);
        if (rc < 0) {
            LOG_ERR("fs_readdir failed: %d", rc);
            break;
        }

        if (entry.name[0] == '\0') {
            break;  // End of directory.
        }

        if (entry.type != FS_DIR_ENTRY_FILE) {
            continue;
        }

        strncpy(sNames[sCount], entry.name, kMaxNameLen - 1);
        sNames[sCount][kMaxNameLen - 1] = '\0';
        sCount++;
    }

    fs_closedir(&dir);

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
