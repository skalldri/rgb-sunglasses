#pragma once

#include <zephyr/fs/fs.h>

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

}  // namespace fs_util
