#include "extension_path.h"

#include <cstddef>
#include <cstring>

namespace extension_path {

namespace {

/* Path separators, matching FatFs rather than intuition: ff.c defines
 *     #define IsSeparator(c) ((c) == '/' || (c) == '\\')
 * so a backslash splits a path just like a slash does. Splitting on '/' alone
 * would let "/NAND:/ext/sub\secret.bin" through this fence and then have FatFs
 * resolve it as "/NAND:/ext/sub/secret.bin" — inside a subdirectory the header
 * documents as denied. */
bool is_separator(char c) { return c == '/' || c == '\\'; }

/* First separator at or after `s`, or nullptr. strchr's counterpart for the
 * two-character separator set above. */
const char *find_separator(const char *s) {
    for (; *s != '\0'; ++s) {
        if (is_separator(*s)) {
            return s;
        }
    }
    return nullptr;
}

/* True if `path` contains a ".." path component. Deliberately checks components
 * rather than a substring, so a legitimate name like "my..ext.llext" is not
 * rejected while "/NAND:/ext/../mcuboot.bin" is. */
bool has_parent_component(const char *path) {
    const char *component = path;

    while (true) {
        const char *sep   = find_separator(component);
        const size_t len  = (sep != nullptr) ? static_cast<size_t>(sep - component)
                                             : strlen(component);

        if (len == 2 && component[0] == '.' && component[1] == '.') {
            return true;
        }

        if (sep == nullptr) {
            return false;
        }
        component = sep + 1;
    }
}

}  // namespace

bool within_dir(const char *path, const char *dir) {
    if (path == nullptr || dir == nullptr) {
        return false;
    }

    /* Reject traversal before anything else: the prefix test below would
     * happily accept "<dir>/../mcuboot.bin", which FATFS then resolves
     * straight out of the fenced directory. */
    if (has_parent_component(path)) {
        return false;
    }

    /* Not a hot path — this runs once per SMP request. */
    const size_t kDirLen = strlen(dir);

    if (strncmp(path, dir, kDirLen) != 0) {
        return false;
    }

    /* A direct child, and only a direct child: exactly one separator, then a
     * non-empty name with no separator of its own. This also rejects the
     * directory itself ("" remainder) and a trailing-separator form. */
    if (!is_separator(path[kDirLen])) {
        return false;
    }

    const char *name = &path[kDirLen + 1];
    if (name[0] == '\0') {
        return false;
    }

    return find_separator(name) == nullptr;
}

}  // namespace extension_path
