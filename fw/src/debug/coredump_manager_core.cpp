#include "coredump_manager_core.h"

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

LOG_MODULE_REGISTER(coredump_mgr, CONFIG_LOG_DEFAULT_LEVEL);

namespace coredump_manager_core {
namespace {

constexpr const char kPrefix[] = "core_";
constexpr const char kSuffix[] = ".bin";

/* Static rather than stack-allocated: FATFS calls already use a good chunk of
 * the (deliberately small) coredump workqueue stack. Only the single manager
 * workqueue thread (or the single-threaded test binary) runs this code. */
uint8_t sChunkBuf[1024];

/* Parses "core_NNNN.bin" (any number of digits) and returns NNNN, or -1 if
 * `name` doesn't match the pattern. */
int parse_dump_index(const char* name) {
    size_t len = strlen(name);
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    constexpr size_t kSuffixLen = sizeof(kSuffix) - 1;
    if (len <= kPrefixLen + kSuffixLen) {
        return -1;
    }
    if (strncmp(name, kPrefix, kPrefixLen) != 0 ||
        strcmp(name + len - kSuffixLen, kSuffix) != 0) {
        return -1;
    }
    // Digits-only between prefix and suffix
    const char* digits = name + kPrefixLen;
    size_t digitCount = len - kPrefixLen - kSuffixLen;
    for (size_t i = 0; i < digitCount; i++) {
        if (digits[i] < '0' || digits[i] > '9') {
            return -1;
        }
    }
    long value = strtol(digits, nullptr, 10);
    if (value < 0 || value > INT32_MAX) {
        return -1;
    }
    return static_cast<int>(value);
}

/* Calls `fn(name)` for every entry in `dir`. Returns 0, or a negative errno if
 * the directory can't be opened/read (missing dir returns -ENOENT). */
template <typename Fn>
int for_each_dir_entry(const char* dir, Fn&& fn) {
    struct fs_dir_t dirp;
    fs_dir_t_init(&dirp);
    int rc = fs_opendir(&dirp, dir);
    if (rc < 0) {
        return rc;
    }
    struct fs_dirent entry;
    while ((rc = fs_readdir(&dirp, &entry)) == 0 && entry.name[0] != '\0') {
        if (entry.type == FS_DIR_ENTRY_FILE) {
            fn(entry.name);
        }
    }
    fs_closedir(&dirp);
    return rc;
}

}  // namespace

int max_dump_index(const char* dir) {
    int maxIndex = -1;
    int rc = for_each_dir_entry(dir, [&maxIndex](const char* name) {
        int index = parse_dump_index(name);
        if (index > maxIndex) {
            maxIndex = index;
        }
    });
    if (rc < 0) {
        return -1;
    }
    return maxIndex;
}

int format_dump_path(char* out, size_t cap, const char* dir, unsigned int index) {
    int written = snprintf(out, cap, "%s/%s%04u%s", dir, kPrefix, index, kSuffix);
    if (written < 0 || static_cast<size_t>(written) >= cap) {
        return -ENOMEM;
    }
    return 0;
}

bool any_dump_files(const char* dir) {
    bool found = false;
    (void)for_each_dir_entry(dir, [&found](const char* name) {
        if (parse_dump_index(name) >= 0) {
            found = true;
        }
    });
    return found;
}

int drain_to_dir(const PartitionOps& ops, const char* dir) {
    int rc = ops.has_dump();
    if (rc < 0) {
        return rc;
    }
    if (rc != 1) {
        return -ENOENT;
    }

    rc = ops.verify();
    if (rc < 0) {
        return rc;
    }
    if (rc != 1) {
        LOG_ERR("stored coredump failed verification — discarding");
        (void)ops.invalidate();
        return -EBADMSG;
    }

    int size = ops.get_size();
    if (size < 0) {
        return size;
    }
    if (size < 2) {
        LOG_ERR("stored coredump implausibly small (%d B) — discarding", size);
        (void)ops.invalidate();
        return -EBADMSG;
    }

    rc = fs_mkdir(dir);
    if (rc < 0 && rc != -EEXIST) {
        return rc;
    }

    char path[64];
    int nextIndex = max_dump_index(dir) + 1;
    rc = format_dump_path(path, sizeof(path), dir, static_cast<unsigned int>(nextIndex));
    if (rc < 0) {
        return rc;
    }

    struct fs_file_t file;
    fs_file_t_init(&file);
    rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
    if (rc < 0) {
        return rc;
    }

    int result = 0;
    for (off_t offset = 0; offset < size;) {
        size_t chunk = MIN(sizeof(sChunkBuf), static_cast<size_t>(size - offset));
        int copied = ops.copy(offset, sChunkBuf, chunk);
        if (copied <= 0) {
            result = (copied < 0) ? copied : -EIO;
            break;
        }
        /* The stored stream must start with Zephyr's coredump file magic
         * ("ZE", coredump_hdr_t) — anything else means the partition holds
         * garbage that would confuse coredump_gdbserver.py. */
        if (offset == 0 && (sChunkBuf[0] != 'Z' || sChunkBuf[1] != 'E')) {
            LOG_ERR("stored coredump has bad magic 0x%02x%02x — discarding", sChunkBuf[0],
                    sChunkBuf[1]);
            (void)ops.invalidate();
            result = -EBADMSG;
            break;
        }
        ssize_t written = fs_write(&file, sChunkBuf, static_cast<size_t>(copied));
        if (written < 0 || static_cast<size_t>(written) != static_cast<size_t>(copied)) {
            result = (written < 0) ? static_cast<int>(written) : -EIO;
            break;
        }
        offset += copied;
    }

    if (result == 0) {
        result = fs_sync(&file);
    }
    fs_close(&file);

    if (result < 0) {
        // Leave the stored dump intact (unless it was garbage) so the next
        // periodic pass retries; don't leave a truncated file behind.
        (void)fs_unlink(path);
        return result;
    }

    // Only invalidate once the file is safely on disk.
    rc = ops.invalidate();
    if (rc < 0) {
        LOG_WRN("coredump saved to %s but invalidate failed (%d); expect a duplicate", path, rc);
    }
    LOG_INF("coredump (%d B) saved to %s", size, path);
    return 0;
}

}  // namespace coredump_manager_core
