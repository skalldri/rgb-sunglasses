#include "extension_file_transfer.h"

#include <extensions/extension_registry.h>

#include <cstddef>
#include <cstring>

/* Everything except the predicate only exists in a firmware build. The
 * native_sim test app compiles this same file for path_allowed() alone and does
 * not set this symbol, so the rest compiles out and the test needs no MCUmgr,
 * Bluetooth or filesystem stack. */
#if defined(CONFIG_APP_EXT_FILE_TRANSFER)
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt_callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>

LOG_MODULE_REGISTER(ext_file_transfer, LOG_LEVEL_INF);
#endif

namespace extension_file_transfer {

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

bool path_allowed(const char *path) {
    if (path == nullptr) {
        return false;
    }

    /* Reject traversal before anything else: the prefix test below would
     * happily accept "/NAND:/ext/../mcuboot.bin", which FATFS then resolves
     * straight out of the fenced directory. */
    if (has_parent_component(path)) {
        return false;
    }

    /* Derived from the registry's own constant rather than a second copy of
     * the literal, so the fence can never drift from the directory the host
     * actually scans. Not a hot path — this runs once per SMP file request. */
    const size_t kDirLen = strlen(extension_registry::kDirectory);

    if (strncmp(path, extension_registry::kDirectory, kDirLen) != 0) {
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

}  // namespace extension_file_transfer

#if defined(CONFIG_APP_EXT_FILE_TRANSFER)

namespace {

enum mgmt_cb_return fs_access_cb(uint32_t event, enum mgmt_cb_return prev_status, int32_t *rc,
                                 uint16_t * /*group*/, bool * /*abort_more*/, void *data,
                                 size_t data_size) {
    if (event != MGMT_EVT_OP_FS_MGMT_FILE_ACCESS) {
        return MGMT_CB_OK;
    }

    /* Don't override an earlier handler's rejection. */
    if (prev_status != MGMT_CB_OK) {
        return prev_status;
    }

    if (data == nullptr || data_size < sizeof(struct fs_mgmt_file_access)) {
        *rc = MGMT_ERR_EACCESSDENIED;
        return MGMT_CB_ERROR_RC;
    }

    const auto *access = static_cast<const struct fs_mgmt_file_access *>(data);

    if (!extension_file_transfer::path_allowed(access->filename)) {
        /* Info rather than warn, and only on the reject path: a denial is a
         * client bug (or an attempt) worth seeing, but this never fires in a
         * steady-state loop the way a per-tick log would. */
        LOG_INF("rejected MCUmgr file access (type %d) outside %s", (int)access->access,
                extension_registry::kDirectory);
        *rc = MGMT_ERR_EACCESSDENIED;
        return MGMT_CB_ERROR_RC;
    }

    return MGMT_CB_OK;
}

struct mgmt_callback fs_access_callback = {
    .callback = fs_access_cb,
    .event_id = MGMT_EVT_OP_FS_MGMT_FILE_ACCESS,
};

/* APPLICATION-phase init: mgmt_callback_register() only touches a static list,
 * so it has no ordering requirement against the FAT mount or the BLE stack —
 * it just has to be in place before the first SMP request can be served, which
 * cannot happen until a peer connects. */
int ext_file_transfer_init() {
    mgmt_callback_register(&fs_access_callback);
    return 0;
}

}  // namespace

SYS_INIT(ext_file_transfer_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_APP_EXT_FILE_TRANSFER */
