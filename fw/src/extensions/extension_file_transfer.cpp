#include "extension_file_transfer.h"

#include <extensions/extension_path.h>
#include <extensions/extension_registry.h>

#include <cstddef>
#include <cstring>

/* Everything except the path_allowed() wrapper only exists in a firmware
 * build. The native_sim test app compiles this file plus extension_path.cpp
 * for the predicate alone and does not set this symbol, so the rest compiles
 * out and the test needs no MCUmgr, Bluetooth or filesystem stack. */
#if defined(CONFIG_APP_EXT_FILE_TRANSFER)
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt_callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>

LOG_MODULE_REGISTER(ext_file_transfer, LOG_LEVEL_INF);
#endif

namespace extension_file_transfer {

/* The predicate logic lives in extension_path.cpp (compiled under
 * CONFIG_APP_EXTENSION_HOST) so the FILE_MGMT custom group keeps the fence
 * even in configs where this file's hook is compiled out. Derived from the
 * registry's own constant rather than a second copy of the literal, so the
 * fence can never drift from the directory the host actually scans. */
bool path_allowed(const char *path) {
    return extension_path::within_dir(path, extension_registry::kDirectory);
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
