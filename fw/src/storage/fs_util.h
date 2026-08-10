#pragma once

#include <zephyr/fs/fs.h>

#include <array>
#include <cstring>

/**
 * @file
 * @brief One directory-walk helper, shared by every consumer that enumerates a
 * FAT directory.
 *
 * Header-only on purpose. A .cpp would have to be added to three separate Twister
 * CMakeLists, and coredump_manager_core.cpp is deliberately dependency-light so its
 * native_sim suite can link it standalone.
 */

namespace fs_util {

/**
 * @brief Calls @p fn once per regular file in @p dir, in filesystem order.
 *
 * @param fn  Invoked as `bool fn(const char *name)`. Return false to stop early
 *            (callers with a fixed-size table use this to stop at capacity).
 *            @p name points into a stack-local `fs_dirent` and is only valid for
 *            the duration of the call — copy it if you need to keep it.
 *
 * @return 0 when the directory was walked to the end (or the callback stopped it),
 *         or a negative errno if the directory could not be opened or a read
 *         failed. A missing directory reports -ENOENT.
 *
 * Non-file entries (subdirectories) are skipped, so callers cannot accidentally
 * treat a directory name as a file. Nothing is logged here: each caller has its own
 * LOG_MODULE and its own idea of whether a failure is worth reporting, so the rc is
 * returned and the messages stay at the call sites.
 */
template <typename Fn>
int for_each_file(const char *dir, Fn &&fn) {
    struct fs_dir_t dirp;
    fs_dir_t_init(&dirp);

    int rc = fs_opendir(&dirp, dir);
    if (rc < 0) {
        return rc;
    }

    struct fs_dirent entry;
    while ((rc = fs_readdir(&dirp, &entry)) == 0) {
        if (entry.name[0] == '\0') {
            break;  // End of directory.
        }
        if (entry.type != FS_DIR_ENTRY_FILE) {
            continue;
        }
        if (!fn(entry.name)) {
            break;
        }
    }

    fs_closedir(&dirp);
    return rc;
}

/** Outcome of collect_names(). */
struct CollectResult {
    /** 0 if the directory was walked to the end, else a negative errno from
     *  fs_opendir/fs_readdir. `names`/`count` still hold whatever was collected
     *  before the failure, already sorted — a partial set is usable, and the
     *  caller decides whether to say so. */
    int rc;
    /** Names retained, sorted ascending. */
    size_t count;
    /** Matching files that did NOT fit in the table. Non-zero means the caller is
     *  serving an incomplete view and should say so — without this, truncation is
     *  indistinguishable from a complete walk. */
    size_t skipped;
};

/** True when @p name ends with @p suffix (case-sensitive, like FAT LFN compares here). */
inline bool has_suffix(const char *name, const char *suffix) {
    const size_t nameLen = strlen(name);
    const size_t suffixLen = strlen(suffix);
    if (nameLen < suffixLen) {
        return false;
    }
    return strcmp(name + (nameLen - suffixLen), suffix) == 0;
}

/**
 * @brief Collects the names in @p dir ending in @p suffix into @p names, sorted.
 *
 * Registries derive slot indices from this ordering, and those indices become
 * extension animation IDs and BLE service UUIDs — so both WHICH names are kept and
 * WHAT ORDER they are in have to be a pure function of the file set, never of FAT
 * directory order. Two properties deliver that, and both matter:
 *
 *  - Insertion is ordered, so the table is sorted at every point, including after
 *    an aborted walk. (A sort bolted on after the loop is skipped by an early
 *    return, which publishes raw FAT order — exactly the bug this shape prevents.)
 *  - When the table is full the LARGEST name is evicted, so an over-full directory
 *    keeps the lexicographically-first @p MaxFiles names. Keeping "the first N the
 *    filesystem happened to hand us" would make the retained set itself vary
 *    between boots, and with it every slot's UUID.
 *
 * The walk continues past capacity rather than stopping, so `skipped` is an exact
 * count instead of a bare "at least one". These directories hold tens of entries;
 * the extra readdir calls are not worth an inexact diagnostic.
 */
template <size_t NameLen, size_t MaxFiles>
CollectResult collect_names(const char *dir, const char *suffix,
                            std::array<char[NameLen], MaxFiles> &names) {
    CollectResult result = {0, 0, 0};

    const auto store = [](char *dest, const char *src) {
        strncpy(dest, src, NameLen - 1);
        dest[NameLen - 1] = '\0';
    };

    result.rc = for_each_file(dir, [&](const char *name) {
        if (!has_suffix(name, suffix)) {
            return true;
        }
        const bool full = (result.count == MaxFiles);
        if (full && strcmp(name, names[MaxFiles - 1]) >= 0) {
            // Sorts at or after the largest name we are keeping: nothing to evict.
            result.skipped++;
            return true;
        }
        // Shift right from the slot that is about to be freed (the tail entry when
        // full — which is therefore dropped — or the first unused slot when not).
        size_t i = full ? MaxFiles - 1 : result.count;
        while (i > 0 && strcmp(names[i - 1], name) > 0) {
            store(names[i], names[i - 1]);
            i--;
        }
        store(names[i], name);
        if (full) {
            result.skipped++;
        } else {
            result.count++;
        }
        return true;  // keep walking so `skipped` is exact
    });

    return result;
}

}  // namespace fs_util
