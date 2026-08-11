#include <storage/fs_util.h>
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

}  // namespace

int scan_dumps(const char* dir, int* out_count, int* out_max) {
    int count = 0;
    int maxIndex = -1;

    int rc = fs_util::for_each_file(dir, [&count, &maxIndex](const char* name) {
        int index = parse_dump_index(name);
        if (index >= 0) {
            count++;
            if (index > maxIndex) {
                maxIndex = index;
            }
        }
        return true;  // walk the whole directory: we want both the total and the max
    });

    if (rc == -ENOENT) {
        /* Directory not created yet — a legitimate "nothing drained so far", not a
         * failure. Report it so callers can tell it apart from a scan that broke, but
         * hand back usable values. */
        *out_count = 0;
        *out_max = -1;
        return -ENOENT;
    }
    if (rc < 0) {
        return rc;
    }

    *out_count = count;
    *out_max = maxIndex;
    return 0;
}

int format_dump_path(char* out, size_t cap, const char* dir, unsigned int index) {
    int written = snprintf(out, cap, "%s/%s%04u%s", dir, kPrefix, index, kSuffix);
    if (written < 0 || static_cast<size_t>(written) >= cap) {
        return -ENOMEM;
    }
    return 0;
}

int drain_to_dir(const PartitionOps& ops, const char* dir, int maxFiles) {
    int rc = ops.has_dump();
    if (rc < 0) {
        return rc;
    }
    if (rc != 1) {
        return -ENOENT;
    }

    /* ONE directory sweep, and it happens BEFORE the verify/mkdir prologue below.
     *
     * Ordering matters at the cap. An at-cap board returns from here every pass, and if
     * that return came after the prologue it would re-checksum the whole capture
     * partition every time AND re-run fs_mkdir() — whose -EEXIST the filesystem layer
     * logs as "failed to create directory (-17)" unconditionally (zephyr/subsys/fs/fs.c).
     * That is an fs-subsystem log line, so no latch in this module can suppress it: the
     * console of an already-crashing board would fill with it once a minute. Before the
     * cap existed this was unreachable, because a successful drain invalidates and the
     * next pass short-circuits at has_dump().
     *
     * A missing directory is fine here (count 0, max -1) — the mkdir below creates it.
     * Any other scan failure is fatal to this pass and must NOT be reported as -ENOENT,
     * which the caller treats as the benign "no dump stored" case; -EIO makes a broken
     * scan visible instead of silently discarding every future dump. */
    int count = 0;
    int maxIndex = -1;
    rc = scan_dumps(dir, &count, &maxIndex);
    if (rc < 0 && rc != -ENOENT) {
        return -EIO;
    }
    if (maxFiles > 0 && count >= maxFiles) {
        return -ENOSPC;
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

    /* maxIndex came from the scan above; a failed scan already returned. Falling back
     * to index 0 on an unreadable directory would overwrite an existing, uncollected
     * dump, which is why that case bails rather than guesses. */
    char path[64];
    int nextIndex = maxIndex + 1;
    rc = format_dump_path(path, sizeof(path), dir, static_cast<unsigned int>(nextIndex));
    if (rc < 0) {
        return rc;
    }

    struct fs_file_t file;
    fs_file_t_init(&file);
    /* FS_O_TRUNC so an unexpected same-name collision replaces the file wholesale
     * rather than leaving stale tail bytes from a longer previous dump — which
     * would be an unparseable coredump. */
    rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
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
