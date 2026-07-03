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
 * like built-in animation services (minus the optional metadata blob, for
 * which the app has a per-descriptor fallback path).
 */

#include <animations/animation_registry.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/gatt_cpf.h>
#include <extensions/extension_bt.h>
#include <extensions/extension_host.h>
#include <pattern_controller.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <cstring>

LOG_MODULE_REGISTER(ext_bt, LOG_LEVEL_INF);

namespace {

using extension_host::kMaxExtensions;

/* Characteristics per service: Animation Name + Is Active + params. */
constexpr size_t kMaxChars = 2 + RGBX_MAX_PARAMS;
/* Attrs per characteristic: declaration + value + CUD + CPF. */
constexpr size_t kMaxAttrs = 1 /* primary service */ + 4 * kMaxChars;

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
    bt_gatt_service svc = {};
};

BtSlot sBtSlots[kMaxExtensions];

/* --- value callbacks ---------------------------------------------------- */

ssize_t read_name(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len,
                  uint16_t offset) {
    const auto *bs = static_cast<const BtSlot *>(attr->user_data);
    const char *name = extension_host::name(bs->slot);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, name, strlen(name));
}

ssize_t read_is_active(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                       uint16_t len, uint16_t offset) {
    const auto *bs = static_cast<const BtSlot *>(attr->user_data);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &bs->isActive, sizeof(bs->isActive));
}

/* Same semantics as AnimationIsActiveBinding::onRemoteActiveChange for the
 * built-ins: write true switches to this animation; write false deactivates
 * it (back to None) only if it is the current one. */
ssize_t write_is_active(struct bt_conn *, const struct bt_gatt_attr *attr, const void *buf,
                        uint16_t len, uint16_t offset, uint8_t) {
    if (offset != 0 || len != 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    const auto *bs = static_cast<const BtSlot *>(attr->user_data);
    const bool active = *static_cast<const uint8_t *>(buf) != 0;
    const Animation id = extension_host::animationId(bs->slot);
    if (active) {
        pattern_controller_change_to_animation(id);
    } else if (pattern_controller_get_current_animation() == id) {
        pattern_controller_change_to_animation(Animation::None);
    }
    return len;
}

ssize_t read_param(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len,
                   uint16_t offset) {
    const auto *ctx = static_cast<const ParamCtx *>(attr->user_data);
    uint32_t value = extension_host::paramValue(ctx->slot, ctx->index);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &value, sizeof(value));
}

ssize_t write_param(struct bt_conn *, const struct bt_gatt_attr *attr, const void *buf,
                    uint16_t len, uint16_t offset, uint8_t) {
    if (offset != 0 || len != sizeof(uint32_t)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    const auto *ctx = static_cast<const ParamCtx *>(attr->user_data);
    uint32_t value;
    memcpy(&value, buf, sizeof(value));
    extension_host::setParamValue(ctx->slot, ctx->index, value);
    return len;
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
    return attrIdx;
}

/* Registry is-active mirror: one setter thunk per slot (the registry takes
 * plain function pointers), keeping the readable is-active value in sync
 * with activation state — the same registry -> characteristic path the
 * built-ins use via IsActiveCharacteristic. */
template <size_t N>
void is_active_setter(bool active) {
    sBtSlots[N].isActive = active ? 1 : 0;
}

constexpr AnimationIsActiveSetter kIsActiveSetters[kMaxExtensions] = {
    is_active_setter<0>,
    is_active_setter<1>,
    is_active_setter<2>,
    is_active_setter<3>,
};
static_assert(kMaxExtensions == 4, "kIsActiveSetters must list one thunk per extension slot");

}  // namespace

void extension_bt_register(size_t slot) {
    if (slot >= kMaxExtensions || !extension_host::isLoaded(slot)) {
        return;
    }
    BtSlot *bs = &sBtSlots[slot];
    if (bs->registered) {
        return;
    }
    bs->slot = slot;

    const uint32_t animId = static_cast<uint32_t>(extension_host::animationId(slot));
    bs->svcUuid = BT_ANIMATION_SERVICE_UUID(animId);

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

    /* Is Active — fixed UUID, read/write, drives activation. */
    attrIdx = append_characteristic(
        bs, attrIdx, chrcIdx++, reinterpret_cast<const bt_uuid *>(&kIsActiveCharacteristicUuid),
        /*writable=*/true, read_is_active, write_is_active, bs, "Is Active", &kCpfBool);

    /* One characteristic per manifest parameter, auto-UUID'd with the same
     * compose scheme BtGattServer uses (characteristic id in the UUID's low
     * bytes; ids start at 1 to keep 0 == the service UUID itself). */
    const size_t nParams = extension_host::paramCount(slot);
    for (size_t p = 0; p < nParams; p++) {
        const extension_host::ParamInfo *info = extension_host::paramInfo(slot, p);
        bs->paramUuids[p] = composeAutoCharacteristicUuid(bs->svcUuid, static_cast<uint16_t>(p + 1));
        bs->paramCtx[p] = {static_cast<uint8_t>(slot), static_cast<uint8_t>(p)};
        const bt_gatt_cpf *cpf = (info->type == RGBX_PARAM_COLOR) ? &kCpfColor : &kCpfUint32;
        attrIdx = append_characteristic(bs, attrIdx, chrcIdx++,
                                        reinterpret_cast<const bt_uuid *>(&bs->paramUuids[p]),
                                        /*writable=*/true, read_param, write_param,
                                        &bs->paramCtx[p], info->name, cpf);
    }

    bs->svc.attrs = bs->attrs;
    bs->svc.attr_count = attrIdx;

    int ret = bt_gatt_service_register(&bs->svc);
    if (ret != 0) {
        LOG_ERR("bt_gatt_service_register failed for slot %zu: %d", slot, ret);
        return;
    }
    bs->registered = true;

    animation_registry_register_is_active(extension_host::animationId(slot),
                                          kIsActiveSetters[slot]);

    LOG_INF("registered BLE service for extension '%s' (%zu attrs)",
            extension_host::name(slot), attrIdx);
}
