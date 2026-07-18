/*
 * extension_bt.cpp — runtime GATT services for sandboxed animation
 * extensions (issue #85).
 *
 * The built-in animations assemble their services at compile time
 * (BtGattServer<...> in bt_service_cpp.h); extensions are discovered at boot,
 * so their attribute tables are assembled here at runtime from static pools
 * and registered via bt_gatt_service_register(). The layout mirrors what
 * BtGattServer emits per characteristic — declaration, value, CUD, CPF — so
 * the companion app's generic discovery treats extension services exactly
 * like built-in animation services, including the optional bulk metadata
 * characteristic (see extension_metadata_blob.h), built at runtime here
 * instead of at compile time the way BtGattServer's MetadataBlobBuilder does.
 */

#include <animations/animation_registry.h>
#include <bluetooth/bt_conn_activity.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/gatt_cpf.h>
#include <extensions/extension_bt.h>
#include <extensions/extension_host.h>
#include <extensions/extension_metadata_blob.h>
#include <pattern_controller.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <array>
#include <cstring>
#include <utility>

LOG_MODULE_REGISTER(ext_bt, LOG_LEVEL_INF);

/* The runtime extension metadata blob (extension_metadata_blob.h) must stay wire-
 * compatible with the compile-time built-in one (bt_service_cpp.h): same version
 * byte, or the app misparses extension blobs / silently drops to slow per-
 * descriptor discovery. This is the one TU that sees both constants, so enforce
 * their equality at compile time rather than by comment. */
static_assert(extension_metadata_blob::kVersion == kMetadataBlobVersion,
              "extension metadata blob version must match the built-in blob version");

namespace {

using extension_host::kMaxExtensions;

/* Characteristics per service: Animation Name + Is Active + params. */
constexpr size_t kMaxChars = 2 + RGBX_MAX_PARAMS;
/* Attrs per characteristic: declaration + value + CUD + CPF; +1 for the CCC
 * on Is Active (the only notifying characteristic); +2 for the bulk metadata
 * characteristic (declaration + value), gated the same way BtGattServer's
 * compile-time equivalent is (issue #90). */
constexpr size_t kMaxAttrs = 1 /* primary service */ + 4 * kMaxChars + 1 +
                             (IS_ENABLED(CONFIG_APP_BT_METADATA_CHARACTERISTIC) ? 2 : 0);

constexpr bt_gatt_cpf kCpfUtf8 = {.format = BLE_GATT_CPF_FORMAT_UTF8S};
constexpr bt_gatt_cpf kCpfBool = {.format = BLE_GATT_CPF_FORMAT_BOOLEAN};
constexpr bt_gatt_cpf kCpfUint32 = {.format = BLE_GATT_CPF_FORMAT_UINT32};
constexpr bt_gatt_cpf kCpfColor = {.format = BLE_GATT_CPF_FORMAT_RGB888};

/* Context handed to per-parameter read/write callbacks via attr->user_data. */
struct ParamCtx {
    uint8_t slot;
    uint8_t index;
};

struct BtSlot {
    size_t slot = 0;
    bool registered = false;
    uint8_t isActive = 0;
    bt_uuid_128 svcUuid = {};
    bt_uuid_128 paramUuids[RGBX_MAX_PARAMS] = {};
    bt_gatt_chrc chrcs[kMaxChars] = {};
    ParamCtx paramCtx[RGBX_MAX_PARAMS] = {};
    bt_gatt_attr attrs[kMaxAttrs] = {};
    bt_gatt_ccc_managed_user_data isActiveCcc = {};
    const bt_gatt_attr *isActiveValueAttr = nullptr;  // notify target
    bt_gatt_service svc = {};
#if defined(CONFIG_APP_BT_METADATA_CHARACTERISTIC)
    /* Bulk metadata blob (issue #90), built once per extension_bt_register()
     * call from the same name/cpf data already fed to append_characteristic()
     * for each characteristic — host-owned static storage, never extension
     * memory (which is untrusted and vanishes on unload). ~390 B/slot, so it's
     * compiled out (not just left unused) when the characteristic is disabled —
     * the extension-host + no-metadata combo the shrunken kMaxAttrs supports. */
    uint8_t metadataBlob[extension_metadata_blob::kMaxBlobSize] = {};
    size_t metadataBlobPos = 0;
    uint8_t metadataEntryCount = 0;
    bt_gatt_chrc metadataChrc = {};
#endif
};

BtSlot sBtSlots[kMaxExtensions];

/* Notifies the CURRENT Is Active value unconditionally. Used both when the
 * value changes and to push a rejection back to a client whose optimistic
 * toggle write didn't take (PR #89 review finding 3) — in that case the
 * value hasn't changed but the app must still hear "you are off". No
 * subscribers is fine; bt_gatt_notify's error is ignored. */
void push_is_active(BtSlot *bs) {
    if (bs->registered && bs->isActiveValueAttr != nullptr) {
        (void)bt_gatt_notify(nullptr, bs->isActiveValueAttr, &bs->isActive,
                             sizeof(bs->isActive));
    }
}

/* --- value callbacks ---------------------------------------------------- */
/* Every handler below notes inbound activity for the conn-param governor
 * (issue #188) - these runtime-built services bypass bt_service_cpp.h's
 * funnels, so they carry their own calls. */

ssize_t read_name(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len,
                  uint16_t offset) {
    bt_conn_activity_note();
    const auto *bs = static_cast<const BtSlot *>(attr->user_data);
    const char *name = extension_host::name(bs->slot);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, name, strlen(name));
}

ssize_t read_is_active(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                       uint16_t len, uint16_t offset) {
    bt_conn_activity_note();
    const auto *bs = static_cast<const BtSlot *>(attr->user_data);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &bs->isActive, sizeof(bs->isActive));
}

#if defined(CONFIG_APP_BT_METADATA_CHARACTERISTIC)
ssize_t read_metadata(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                      uint16_t len, uint16_t offset) {
    bt_conn_activity_note();
    const auto *bs = static_cast<const BtSlot *>(attr->user_data);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, bs->metadataBlob, bs->metadataBlobPos);
}
#endif

/* Same semantics as AnimationIsActiveBinding::onRemoteActiveChange for the
 * built-ins: write true switches to this animation; write false deactivates
 * it (back to None) only if it is the current one. Unlike built-ins,
 * activation can FAIL:
 *  - a FAULTED slot rejects the write with an ATT error, so the app's own
 *    optimistic-update revert turns the toggle back off (a success + notify
 *    pushback loses a race against the app's optimistic update, which lands
 *    AFTER the write response and clobbers the notified value — observed on
 *    hardware);
 *  - a sandbox that dies later (the load is deferred to the first tick)
 *    is reported by the fault path's Is Active = false notification.
 * Only `ext select` (deliberate developer action) resets a faulted slot. */
ssize_t write_is_active(struct bt_conn *, const struct bt_gatt_attr *attr, const void *buf,
                        uint16_t len, uint16_t offset, uint8_t) {
    bt_conn_activity_note();
    if (offset != 0 || len != 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    auto *bs = static_cast<BtSlot *>(attr->user_data);
    const bool active = *static_cast<const uint8_t *>(buf) != 0;
    const Animation id = extension_host::animationId(bs->slot);
    if (active) {
        if (extension_host::isFaulted(bs->slot)) {
            return BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED);
        }
        pattern_controller_change_to_animation(id);
        if (bs->isActive == 0) {
            /* The switch was refused synchronously (e.g. raced a concurrent
             * fault): reject so the app reverts. */
            return BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED);
        }
    } else if (pattern_controller_get_current_animation() == id) {
        pattern_controller_change_to_animation(Animation::None);
    }
    return len;
}

/* Both callbacks dispatch on the validated param type, mirroring the wire
 * conventions of the equivalent built-in characteristic types
 * (bt_gatt_traits.h / bt_service_cpp.h): BOOL = 1 byte, STRING = UTF-8 up to
 * RGBX_PARAM_STRING_MAX-1 bytes with a forced NUL, UINT32/COLOR = 4-byte LE. */
ssize_t read_param(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len,
                   uint16_t offset) {
    bt_conn_activity_note();
    const auto *ctx = static_cast<const ParamCtx *>(attr->user_data);
    const extension_host::ParamInfo *info = extension_host::paramInfo(ctx->slot, ctx->index);
    if (info == nullptr) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    switch (info->type) {
        case RGBX_PARAM_BOOL: {
            const uint8_t value = extension_host::paramValue(ctx->slot, ctx->index) ? 1 : 0;
            return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
        }
        case RGBX_PARAM_STRING: {
            const char *value = extension_host::paramString(ctx->slot, ctx->index);
            return bt_gatt_attr_read(conn, attr, buf, len, offset, value, strlen(value));
        }
        default: {
            const uint32_t value = extension_host::paramValue(ctx->slot, ctx->index);
            return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
        }
    }
}

ssize_t write_param(struct bt_conn *, const struct bt_gatt_attr *attr, const void *buf,
                    uint16_t len, uint16_t offset, uint8_t flags) {
    bt_conn_activity_note();
    const auto *ctx = static_cast<const ParamCtx *>(attr->user_data);
    const extension_host::ParamInfo *info = extension_host::paramInfo(ctx->slot, ctx->index);
    if (info == nullptr) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    switch (info->type) {
        case RGBX_PARAM_BOOL: {
            if (offset != 0 || len != 1) {
                return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
            }
            extension_host::setParamValue(ctx->slot, ctx->index,
                                          *static_cast<const uint8_t *>(buf) ? 1 : 0);
            return len;
        }
        case RGBX_PARAM_STRING: {
            /* Same long-write handling as the built-in string
             * characteristics (bt_service_cpp.h _write): allow the prepare
             * phase, then bounds-check data + forced NUL against the value
             * buffer. */
            if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
                return 0;
            }
            if (!extension_host::writeParamString(ctx->slot, ctx->index, offset, buf, len)) {
                return BT_GATT_ERR(offset != 0 ? BT_ATT_ERR_INVALID_OFFSET
                                               : BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
            }
            return len;
        }
        default: {
            if (offset != 0 || len != sizeof(uint32_t)) {
                return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
            }
            uint32_t value;
            memcpy(&value, buf, sizeof(value));
            extension_host::setParamValue(ctx->slot, ctx->index, value);
            return len;
        }
    }
}

/* --- runtime attribute assembly ----------------------------------------- */

/* Appends one characteristic (declaration + value + CUD + CPF) to bs->attrs,
 * mirroring BtGattServer's per-characteristic layout. `cud` and `cpf` must
 * point at storage that outlives the service (static pools / host copies). */
size_t append_characteristic(BtSlot *bs, size_t attrIdx, size_t chrcIdx, const bt_uuid *uuid,
                             bool writable, bt_gatt_attr_read_func_t read,
                             bt_gatt_attr_write_func_t write, void *userData, const char *cud,
                             const bt_gatt_cpf *cpf) {
    bt_gatt_chrc *chrc = &bs->chrcs[chrcIdx];
    chrc->uuid = uuid;
    chrc->value_handle = 0;
    chrc->properties = BT_GATT_CHRC_READ | (writable ? BT_GATT_CHRC_WRITE : 0);

    bs->attrs[attrIdx++] = {
        .uuid = BT_UUID_GATT_CHRC,
        .read = bt_gatt_attr_read_chrc,
        .write = nullptr,
        .user_data = chrc,
        .handle = 0,
        .perm = BT_GATT_PERM_READ,
    };
    bs->attrs[attrIdx++] = {
        .uuid = uuid,
        .read = read,
        .write = write,
        .user_data = userData,
        .handle = 0,
        .perm = static_cast<uint16_t>(
            BT_GATT_PERM_READ_ENCRYPT |
            (writable ? BT_GATT_PERM_WRITE_ENCRYPT | BT_GATT_PERM_PREPARE_WRITE : 0)),
    };
    bs->attrs[attrIdx++] = {
        .uuid = BT_UUID_GATT_CUD,
        .read = bt_gatt_attr_read_cud,
        .write = nullptr,
        .user_data = const_cast<char *>(cud),
        .handle = 0,
        .perm = BT_GATT_PERM_READ,
    };
    bs->attrs[attrIdx++] = {
        .uuid = BT_UUID_GATT_CPF,
        .read = bt_gatt_attr_read_cpf,
        .write = nullptr,
        .user_data = const_cast<bt_gatt_cpf *>(cpf),
        .handle = 0,
        .perm = BT_GATT_PERM_READ,
    };

    /* Bulk metadata blob entry (issue #90): written from the same cud/cpf
     * already fed into this characteristic's CUD/CPF attrs above, so blob
     * order can never drift from attr/handle order — no second pass over
     * the characteristic list is needed to rebuild it. */
#if defined(CONFIG_APP_BT_METADATA_CHARACTERISTIC)
    if (extension_metadata_blob::append(bs->metadataBlob, sizeof(bs->metadataBlob),
                                        &bs->metadataBlobPos, cpf->format, cud)) {
        bs->metadataEntryCount++;
    }
#endif
    return attrIdx;
}

/* Registry is-active mirror: one setter thunk per slot (the registry takes
 * plain function pointers), keeping the readable is-active value in sync
 * with activation state and notifying subscribers on change — the same
 * registry -> characteristic path the built-ins use via
 * IsActiveCharacteristic. The thunk table is expanded from kMaxExtensions
 * via index_sequence so a capacity change needs no hand edits here (PR #89
 * review finding 7). */
template <size_t N>
void is_active_setter(bool active) {
    BtSlot *bs = &sBtSlots[N];
    const uint8_t value = active ? 1 : 0;
    if (bs->isActive != value) {
        bs->isActive = value;
        push_is_active(bs);
    }
}

template <size_t... I>
constexpr std::array<AnimationIsActiveSetter, sizeof...(I)> make_is_active_setters(
    std::index_sequence<I...>) {
    return {{is_active_setter<I>...}};
}

constexpr auto kIsActiveSetters = make_is_active_setters(std::make_index_sequence<kMaxExtensions>{});

}  // namespace

int extension_bt_register(size_t slot) {
    if (slot >= kMaxExtensions || !extension_host::isLoaded(slot)) {
        return -EINVAL;
    }
    BtSlot *bs = &sBtSlots[slot];
    if (bs->registered) {
        return 0;
    }
    bs->slot = slot;

    /* BT_ANIMATION_SERVICE_UUID(animId) can't take a runtime id: every byte
     * of its braced initializer becomes a non-constant int -> uint8_t
     * conversion, i.e. a -Wnarrowing warning (issue #164). Start from the
     * constexpr id-0 UUID and patch in the id group exactly where the macro
     * would put it: w16_3 of BT_UUID_128_ENCODE lands at val[6..7],
     * little-endian (see zephyr/bluetooth/uuid.h). */
    const uint16_t animId = static_cast<uint16_t>(extension_host::animationId(slot));
    bs->svcUuid = BT_ANIMATION_SERVICE_UUID(0);
    sys_put_le16(static_cast<uint16_t>(animId << 8), &bs->svcUuid.val[6]);

    /* Reset the metadata blob on every (re)registration — a slot can be
     * re-registered after unload with a different manifest, so stale
     * entries from a previous registration must not linger. */
#if defined(CONFIG_APP_BT_METADATA_CHARACTERISTIC)
    extension_metadata_blob::init(bs->metadataBlob);
    bs->metadataBlobPos = 2;
    bs->metadataEntryCount = 0;
#endif

    size_t attrIdx = 0;
    size_t chrcIdx = 0;

    bs->attrs[attrIdx++] = {
        .uuid = BT_UUID_GATT_PRIMARY,
        .read = bt_gatt_attr_read_service,
        .write = nullptr,
        .user_data = &bs->svcUuid,
        .handle = 0,
        .perm = BT_GATT_PERM_READ,
    };

    /* Animation Name — same fixed UUID as every built-in animation service,
     * so the app identifies it the same way. */
    attrIdx = append_characteristic(bs, attrIdx, chrcIdx++,
                                    reinterpret_cast<const bt_uuid *>(&kAnimationNameCharacteristicUuid),
                                    /*writable=*/false, read_name, nullptr, bs, "Animation Name",
                                    &kCpfUtf8);

    /* Is Active — fixed UUID, read/write/notify, drives activation. The
     * notify path lets the firmware push Is Active = false when a sandbox
     * dies or a write is rejected, so the app disables the toggle (built-ins
     * notify too, via IsActiveCharacteristic). CCC follows CPF, mirroring
     * BtGattCharacteristicCommon's attribute order. */
    const size_t isActiveChrc = chrcIdx;
    attrIdx = append_characteristic(
        bs, attrIdx, chrcIdx++, reinterpret_cast<const bt_uuid *>(&kIsActiveCharacteristicUuid),
        /*writable=*/true, read_is_active, write_is_active, bs, "Is Active", &kCpfBool);
    bs->chrcs[isActiveChrc].properties |= BT_GATT_CHRC_NOTIFY;
    bs->isActiveValueAttr = &bs->attrs[attrIdx - 3];  // value attr of the 4 just appended
    bs->isActiveCcc = BT_GATT_CCC_MANAGED_USER_DATA_INIT(nullptr, nullptr, nullptr);
    bs->attrs[attrIdx++] = {
        .uuid = BT_UUID_GATT_CCC,
        .read = bt_gatt_attr_read_ccc,
        .write = bt_gatt_attr_write_ccc,
        .user_data = &bs->isActiveCcc,
        .handle = 0,
        .perm = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
    };

    /* One characteristic per manifest parameter, auto-UUID'd with the same
     * compose scheme BtGattServer uses (characteristic id in the UUID's low
     * bytes; ids start at 1 to keep 0 == the service UUID itself). */
    const size_t nParams = extension_host::paramCount(slot);
    for (size_t p = 0; p < nParams; p++) {
        const extension_host::ParamInfo *info = extension_host::paramInfo(slot, p);
        bs->paramUuids[p] = composeAutoCharacteristicUuid(bs->svcUuid, static_cast<uint16_t>(p + 1));
        bs->paramCtx[p] = {static_cast<uint8_t>(slot), static_cast<uint8_t>(p)};
        const bt_gatt_cpf *cpf;
        switch (info->type) {
            case RGBX_PARAM_COLOR:
                cpf = &kCpfColor;
                break;
            case RGBX_PARAM_BOOL:
                cpf = &kCpfBool;
                break;
            case RGBX_PARAM_STRING:
                cpf = &kCpfUtf8;
                break;
            default:
                cpf = &kCpfUint32;
                break;
        }
        attrIdx = append_characteristic(bs, attrIdx, chrcIdx++,
                                        reinterpret_cast<const bt_uuid *>(&bs->paramUuids[p]),
                                        /*writable=*/true, read_param, write_param,
                                        &bs->paramCtx[p], info->name, cpf);
    }

    /* Bulk metadata characteristic (issue #90): appended AFTER every other
     * provider attr so it never shifts an earlier characteristic's handle —
     * mirrors BtGattServer's getMetadataAttrsTuple() ordering rule. Reuses
     * the same fixed UUID/version as the compile-time mechanism so the
     * app's existing lookup finds it with zero app-side changes. */
#if defined(CONFIG_APP_BT_METADATA_CHARACTERISTIC)
    bs->metadataBlobPos =
        extension_metadata_blob::finish(bs->metadataBlob, bs->metadataBlobPos, bs->metadataEntryCount);

    bs->metadataChrc.uuid = &kMetadataCharacteristicUuid.uuid;
    bs->metadataChrc.value_handle = 0;
    bs->metadataChrc.properties = BT_GATT_CHRC_READ;

    bs->attrs[attrIdx++] = {
        .uuid = BT_UUID_GATT_CHRC,
        .read = bt_gatt_attr_read_chrc,
        .write = nullptr,
        .user_data = &bs->metadataChrc,
        .handle = 0,
        .perm = BT_GATT_PERM_READ,
    };
    bs->attrs[attrIdx++] = {
        .uuid = &kMetadataCharacteristicUuid.uuid,
        .read = read_metadata,
        .write = nullptr,
        .user_data = bs,
        .handle = 0,
        .perm = BT_GATT_PERM_READ_ENCRYPT,
    };
#endif

    bs->svc.attrs = bs->attrs;
    bs->svc.attr_count = attrIdx;

    int ret = bt_gatt_service_register(&bs->svc);
    if (ret != 0) {
        LOG_ERR("bt_gatt_service_register failed for slot %zu: %d", slot, ret);
        return ret;
    }
    bs->registered = true;

    LOG_INF("registered BLE service for extension '%s' (%zu attrs)",
            extension_host::name(slot), attrIdx);
    return 0;
}

int extension_bt_bind_is_active(size_t slot) {
    if (slot >= kMaxExtensions || !sBtSlots[slot].registered) {
        return -EINVAL;
    }
    /* Must run AFTER extension_animation_proxy_register():
     * animation_registry_register_is_active refuses (-ENOENT) ids that have
     * no registry entry yet, and the proxy's registration is what creates
     * it. (Learned the hard way — binding from extension_bt_register(),
     * which runs before the proxy for rollback reasons, silently left the
     * Is Active mirror dead: reads returned 0 for the active extension and
     * fault notifications never fired.) */
    return animation_registry_register_is_active(extension_host::animationId(slot),
                                                 kIsActiveSetters[slot]);
}

void extension_bt_unregister(size_t slot) {
    if (slot >= kMaxExtensions) {
        return;
    }
    BtSlot *bs = &sBtSlots[slot];
    if (!bs->registered) {
        return;
    }
    (void)bt_gatt_service_unregister(&bs->svc);
    bs->registered = false;
}
