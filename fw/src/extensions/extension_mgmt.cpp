/*
 * extension_mgmt — FILE_MGMT custom SMP group (id 64): LIST + DELETE for
 * managed on-device file stores. Design and wire format:
 * fw/docs/extension-management.md; the enums it shares with the app live in
 * extension_mgmt.h.
 *
 * Runs on the MCUmgr SMP workqueue — a preemptible kernel thread (priority
 * CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_THREAD_PRIO, default 3), the same
 * context fs_mgmt's own flash I/O uses, so fs_* calls here respect the
 * no-flash-I/O-from-cooperative-threads rule. Stack budget
 * (CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_STACK_SIZE — 4096 on proto0, an
 * invariant documented in fw/docs/threading.md) is the resource to guard:
 * exactly one struct fs_dirent (~264 B with FATFS LFN) lives on it at a
 * time, never an array — see the MCUMGR_GRP_FS_DL_CHUNK_SIZE stack-overflow
 * precedent documented in the board conf.
 *
 * Fencing: this group does NOT pass through fs_mgmt's
 * MGMT_EVT_OP_FS_MGMT_FILE_ACCESS hook (that event is emitted only inside
 * fs_mgmt), so every path is validated directly with the same pure predicate
 * the hook's fence uses (extension_path::within_dir — covered by the
 * extensions.file_transfer native_sim suite).
 */

#include "extension_mgmt.h"

#include <extensions/extension_host.h>
#include <extensions/extension_limits.h>
#include <extensions/extension_path.h>
#include <extensions/extension_registry.h>
#include <pattern_controller.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>

#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

LOG_MODULE_REGISTER(ext_mgmt, LOG_LEVEL_INF);

namespace {

using extension_mgmt::Error;

/* ZCBOR_MAP_DECODE_KEY_DECODER is C-only (mixed designated/positional
 * initializers, implicit const char* -> const uint8_t*), so build the
 * key/decoder records with a C++ helper instead. Same layout, same
 * zcbor_map_decode_bulk() consumer. */
template <typename Decoder>
zcbor_map_decode_key_val map_key(const char *key, size_t keyLen, Decoder dec, void *valuePtr) {
    zcbor_map_decode_key_val kv = {};
    kv.key.value = reinterpret_cast<const uint8_t *>(key);
    kv.key.len = keyLen;
    kv.decoder = reinterpret_cast<zcbor_decoder_t *>(dec);
    kv.value_ptr = valuePtr;
    kv.found = false;
    return kv;
}

/* Entries per LIST response. 8 × ~60 B of CBOR ≈ 500 B — comfortably inside
 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE (2048) with header/err headroom, and
 * small enough that a shrunk netbuf degrades to more pages, not failure. */
constexpr size_t kListPageMax = 8;

/* v1 implements only the "ext" kind; "glim" is reserved (see the header).
 * Returns the kind's directory, or nullptr for KIND_UNSUPPORTED. */
const char *dir_for_kind(const struct zcbor_string &kind) {
    if (kind.len == 3 && memcmp(kind.value, "ext", 3) == 0) {
        return extension_registry::kDirectory;
    }
    return nullptr;
}

bool encode_err(zcbor_state_t *zse, Error err) {
    return smp_add_cmd_err(zse, extension_mgmt::kGroupId, static_cast<int>(err));
}

/* Longest file NAME (not path) DELETE accepts and LIST reports — sized to
 * FatFs LFN (CONFIG_FS_FATFS_MAX_LFN = 255), NOT the registry's 32-byte
 * loadable-name bound: files the registry can't load (over-long junk) are
 * exactly what this group must still be able to name and delete. LIST used
 * to truncate names at 32 bytes while DELETE rejected them at the same
 * bound, making long-named files listable-but-undeletable forever. */
constexpr size_t kMaxWireNameLen = 256;

/* Builds "<dir>/<name>" and validates it against the fence. Returns false on
 * any invalid/out-of-fence name. `name` need not be NUL-terminated
 * (zcbor_string); its length is validated against the wire bound first so
 * the copy can't truncate silently. */
bool build_fenced_path(const char *dir, const struct zcbor_string &name, char *out, size_t outLen) {
    if (name.len == 0 || name.len >= kMaxWireNameLen) {
        return false;
    }

    const int written = snprintf(out, outLen, "%s/%.*s", dir, static_cast<int>(name.len),
                                 reinterpret_cast<const char *>(name.value));
    if (written < 0 || static_cast<size_t>(written) >= outLen) {
        return false;
    }

    return extension_path::within_dir(out, dir);
}

/* One LIST entry. Slot annotations (display name, slot, faulted, active,
 * retired) are only emitted for boot-snapshot slots (`loaded` true); `slot`
 * is -1 otherwise. */
bool encode_entry(zcbor_state_t *zse, const char *name, bool onDisk, int slot) {
    const bool loaded = slot >= 0;
    bool ok = zcbor_map_start_encode(zse, 8) &&
              zcbor_tstr_put_lit(zse, "n") &&
              /* Full name up to the wire bound — a truncated name would
               * round-trip into a DELETE that can't match the file. */
              zcbor_tstr_put_term(zse, name, kMaxWireNameLen) &&
              zcbor_tstr_put_lit(zse, "disk") &&
              zcbor_bool_put(zse, onDisk) &&
              zcbor_tstr_put_lit(zse, "loaded") &&
              zcbor_bool_put(zse, loaded);

    if (ok && loaded) {
        const auto uslot = static_cast<size_t>(slot);
        const char *display = extension_host::name(uslot);
        ok = zcbor_tstr_put_lit(zse, "d") &&
             zcbor_tstr_put_term(zse, display != nullptr ? display : "",
                                 extension_host::kMaxNameLen) &&
             zcbor_tstr_put_lit(zse, "s") &&
             zcbor_uint32_put(zse, static_cast<uint32_t>(uslot)) &&
             zcbor_tstr_put_lit(zse, "f") &&
             zcbor_bool_put(zse, extension_host::isFaulted(uslot)) &&
             zcbor_tstr_put_lit(zse, "a") &&
             zcbor_bool_put(zse, extension_host::activeSlot() == slot) &&
             zcbor_tstr_put_lit(zse, "r") &&
             zcbor_bool_put(zse, extension_host::isRetired(uslot));
    }

    return ok && zcbor_map_end_encode(zse, 8);
}

/* True if the slot's file is currently present on disk — used to find the
 * "loaded but deleted/replaced since boot" ghosts for LIST's union. One
 * fs_stat per slot, bounded by kMaxExtensions. */
bool slot_file_on_disk(size_t slot) {
    char path[64];
    const char *fileName = extension_host::fileName(slot);
    if (fileName == nullptr) {
        return false;
    }
    const int written =
        snprintf(path, sizeof(path), "%s/%s", extension_registry::kDirectory, fileName);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(path)) {
        return false;
    }
    struct fs_dirent entry;
    return fs_stat(path, &entry) == 0;
}

/*
 * Command handler: LIST (read).
 *
 * The enumeration is a single stable index space per boot: first every
 * directory entry in FAT readdir order, then every boot-snapshot slot whose
 * file is no longer on disk ("ghosts"). Pagination walks that space with
 * `off`; the app re-lists from 0 after any mutation of its own rather than
 * trusting cross-call ordering (documented in the design).
 */
int list_handler(struct smp_streamer *ctxt) {
    zcbor_state_t *zse = ctxt->writer->zs;
    zcbor_state_t *zsd = ctxt->reader->zs;

    struct zcbor_string kind = {};
    uint32_t off = 0;
    size_t decoded = 0;
    struct zcbor_map_decode_key_val list_decode[] = {
        map_key("kind", 4, zcbor_tstr_decode, &kind),
        map_key("off", 3, zcbor_uint32_decode, &off),
    };

    if (zcbor_map_decode_bulk(zsd, list_decode, ARRAY_SIZE(list_decode), &decoded) != 0 ||
        kind.len == 0) {
        return MGMT_ERR_EINVAL;
    }

    const char *dir = dir_for_kind(kind);
    if (dir == nullptr) {
        return encode_err(zse, Error::kKindUnsupported) ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
    }

    bool ok = zcbor_tstr_put_lit(zse, "entries") &&
              zcbor_list_start_encode(zse, kListPageMax);
    if (!ok) {
        return MGMT_ERR_EMSGSIZE;
    }

    uint32_t index = 0;    // position in the combined enumeration
    size_t encoded = 0;    // entries emitted into this page
    bool hasMore = false;  // an entry beyond this page exists

    /* Phase 1: disk truth. */
    struct fs_dir_t dirp;
    fs_dir_t_init(&dirp);
    int rc = fs_opendir(&dirp, dir);
    if (rc == 0) {
        while (true) {
            /* The ONE dirent on this stack (see the file-top comment). */
            struct fs_dirent entry;
            rc = fs_readdir(&dirp, &entry);
            if (rc != 0 || entry.name[0] == '\0') {
                break;
            }
            if (entry.type == FS_DIR_ENTRY_DIR) {
                continue;
            }
            if (index >= off) {
                if (encoded >= kListPageMax) {
                    hasMore = true;
                    break;
                }
                /* A file matching a RETIRED slot is a re-upload over a
                 * deleted name: the meaningful state is "fresh file, takes
                 * effect after restart", not "removed" — reporting the ghost
                 * slot here made a just-installed extension look deleted. */
                int slot = extension_host::findSlotByFileName(entry.name);
                if (slot >= 0 && extension_host::isRetired(static_cast<size_t>(slot))) {
                    slot = -1;
                }
                ok = encode_entry(zse, entry.name, /*onDisk=*/true, slot);
                if (!ok) {
                    break;
                }
                encoded++;
            }
            index++;
        }
        fs_closedir(&dirp);
    } else {
        LOG_WRN("LIST: fs_opendir(%s) failed: %d", dir, rc);
    }

    /* Phase 2: ghosts — loaded at boot, no longer on disk. */
    if (ok && !hasMore) {
        for (size_t slot = 0; slot < extension_host::count(); slot++) {
            if (slot_file_on_disk(slot)) {
                continue;
            }
            if (index >= off) {
                if (encoded >= kListPageMax) {
                    hasMore = true;
                    break;
                }
                const char *fileName = extension_host::fileName(slot);
                ok = encode_entry(zse, fileName != nullptr ? fileName : "", /*onDisk=*/false,
                                  static_cast<int>(slot));
                if (!ok) {
                    break;
                }
                encoded++;
            }
            index++;
        }
    }

    ok = ok && zcbor_list_end_encode(zse, kListPageMax);

    if (ok && hasMore) {
        ok = zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, off + encoded);
    }

    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/*
 * Command handler: DELETE (write).
 *
 * Order matters (design §3.3/§3.4), and it is retire-first:
 *
 *  1. retire() the boot slot — from here no new activation can queue a lazy
 *     load of the file that is about to disappear (a post-unlink retire left
 *     a window where shuffle or a GATT write could accept an activation
 *     whose FAT read then finds nothing and faults the slot unrecoverably).
 *  2. unlinkQuiesced() — the unlink runs under the host lock, so an ALREADY
 *     in-flight llext load of this file finishes before its clusters are
 *     freed. FatFs here is FF_FS_LOCK=0 and non-reentrant: an unsynchronized
 *     unlink SUCCEEDS against an open file and frees the cluster chain
 *     mid-read, corrupting /NAND: for unrelated files too.
 *  3. On unlink FAILURE, unretire() — a failed delete stays a true no-op.
 *  4. On success, switch away if the slot backs the currently rendered
 *     animation. The test is against pattern_controller's current animation,
 *     NOT activeSlot(): a FAULTED extension has activeSlot() == -1 while its
 *     FAULT banner still renders, and deleting it must clear that banner
 *     rather than leave it scrolling a deleted extension's name forever.
 *  5. Purge persisted settings (async — never blocks the SMP thread).
 */
int delete_handler(struct smp_streamer *ctxt) {
    zcbor_state_t *zse = ctxt->writer->zs;
    zcbor_state_t *zsd = ctxt->reader->zs;

    struct zcbor_string kind = {};
    struct zcbor_string name = {};
    size_t decoded = 0;
    struct zcbor_map_decode_key_val delete_decode[] = {
        map_key("kind", 4, zcbor_tstr_decode, &kind),
        map_key("name", 4, zcbor_tstr_decode, &name),
    };

    if (zcbor_map_decode_bulk(zsd, delete_decode, ARRAY_SIZE(delete_decode), &decoded) != 0 ||
        kind.len == 0) {
        return MGMT_ERR_EINVAL;
    }

    const char *dir = dir_for_kind(kind);
    if (dir == nullptr) {
        return encode_err(zse, Error::kKindUnsupported) ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
    }

    /* Static: ~520 B of LFN-sized buffers (kMaxWireNameLen) have no place
     * on the SMP workqueue stack (4096 B on proto0, an invariant sized for
     * the switch-away path below — fw/docs/threading.md), and SMP handlers
     * are single-threaded by construction (one smp_work_queue), so one
     * shared buffer is safe. */
    static char path[sizeof("/NAND:/ext/") + kMaxWireNameLen];
    static char fileName[kMaxWireNameLen];
    if (!build_fenced_path(dir, name, path, sizeof(path))) {
        return encode_err(zse, Error::kInvalidName) ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
    }

    /* NUL-terminated copy of just the file name for the slot lookup. */
    memcpy(fileName, name.value, name.len);
    fileName[name.len] = '\0';

    /* A slot that is ALREADY retired is a previous DELETE's leftover (or a
     * re-upload over one) — this DELETE targets only the on-disk file, and
     * must not re-run (or roll back) that slot's lifecycle. */
    int slot = extension_host::findSlotByFileName(fileName);
    if (slot >= 0 && extension_host::isRetired(static_cast<size_t>(slot))) {
        slot = -1;
    }

    if (slot >= 0) {
        extension_host::retire(static_cast<size_t>(slot));
    }

    int rc = extension_host::unlinkQuiesced(path);
    if (rc != 0) {
        if (slot >= 0) {
            extension_host::unretire(static_cast<size_t>(slot));
        }
        LOG_WRN("DELETE %s failed: %d", path, rc);
        const Error err = (rc == -ENOENT) ? Error::kNotFound : Error::kUnlinkFailed;
        return encode_err(zse, err) ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
    }

    if (slot >= 0 &&
        pattern_controller_get_current_animation() ==
            extension_host::animationId(static_cast<size_t>(slot))) {
        /* The deleted slot backs whatever is on the glasses right now — a
         * healthy render OR a fault banner (see the handler comment, step 4).
         * Switch to the boot-fallback built-in, persist included, so neither
         * the render loop nor the next boot points at the removed file. Runs
         * on this (SMP workqueue) thread — pattern_controller_change_to_animation
         * is documented caller-thread-safe (shell and BT RX already do this),
         * and the SMP workqueue stack is sized for the switch path (see
         * CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_STACK_SIZE in the board conf). */
        pattern_controller_change_to_animation(Animation::ZigZag, true);
    }

    if (slot >= 0) {
        extension_host::purgePersistence(static_cast<size_t>(slot));
    }

    LOG_INF("deleted %s%s", path, slot >= 0 ? " (slot retired until reboot)" : "");
    return MGMT_ERR_EOK;
}

/* Indexed by command id — positional because C++ has no array designators:
 * [kCmdList]=LIST(read), [kCmdDelete]=DELETE(write). */
const struct mgmt_handler handlers[] = {
    {.mh_read = list_handler, .mh_write = nullptr},
    {.mh_read = nullptr, .mh_write = delete_handler},
};
static_assert(extension_mgmt::kCmdList == 0 && extension_mgmt::kCmdDelete == 1,
              "handlers[] is positional over the command ids");

#if defined(CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL)
/* Legacy-protocol clients get the closest MGMT_ERR_*; the app always speaks
 * SMPv2 and sees the group-specific codes from extension_mgmt.h. */
int translate_error(uint16_t rc) {
    switch (static_cast<Error>(rc)) {
        case Error::kKindUnsupported:
            return MGMT_ERR_ENOTSUP;
        case Error::kInvalidName:
            return MGMT_ERR_EINVAL;
        case Error::kNotFound:
            return MGMT_ERR_ENOENT;
        default:
            return MGMT_ERR_EUNKNOWN;
    }
}
#endif

struct mgmt_group file_mgmt_group = {
    .mg_handlers = handlers,
    .mg_handlers_count = ARRAY_SIZE(handlers),
    .mg_group_id = extension_mgmt::kGroupId,
#if defined(CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL)
    .mg_translate_error = translate_error,
#endif
#if defined(CONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME)
    .mg_group_name = "file mgmt",
#endif
};

void file_mgmt_register(void) {
    mgmt_register_group(&file_mgmt_group);
}

}  // namespace

/* Token must not collide with the extension_mgmt namespace — the macro
 * declares a global variable with this exact name. */
MCUMGR_HANDLER_DEFINE(file_mgmt, file_mgmt_register);
