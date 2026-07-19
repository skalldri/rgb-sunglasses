/*
 * Tests for the runtime extension GATT service assembler
 * (fw/src/extensions/extension_bt.cpp, issue #180), compiled with the real
 * BT host headers + CONFIG_BT_GATT_DYNAMIC_DB on native_sim.
 *
 * extension_host.cpp (llext/FAT-heavy) and pattern_controller.cpp
 * (LED-controller/thread-heavy) are NOT linked here - both are faked below,
 * matching their real headers' declared signatures exactly, so the linker
 * resolves extension_bt.cpp's calls into these fakes instead. Everything
 * else extension_bt.cpp depends on (extension_metadata_blob.cpp,
 * animation_registry.cpp, animation_base.cpp) is Zephyr-free/lightweight
 * enough to compile for real, same rationale as their own native_sim suites.
 *
 * bt_gatt_service_register()/unregister() work without bt_enable() (see
 * gatt.c: the dynamic-db path only checks CONFIG_BT_SETTINGS gating and
 * GATT_INITIALIZED, neither of which this test sets), so the registered
 * attribute table is inspected via the real bt_gatt_foreach_attr_type() API
 * - the same mechanism BT_ATT read/write handling itself uses - rather than
 * reaching into extension_bt.cpp's private BtSlot state.
 */

#include <animations/animation_registry.h>
#include <animations/animation_types.h>
#include <bluetooth/bt_service_cpp.h>
#include <extensions/extension_bt.h>
#include <extensions/extension_host.h>
#include <extensions/extension_metadata_blob.h>
#include <pattern_controller.h>
#include <rgbx/rgbx_api.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <cerrno>
#include <cstring>

/* ---- Fakes standing in for extension_host.cpp -------------------------- */

namespace {

struct FakeSlot {
    bool loaded = false;
    bool faulted = false;
    char name[extension_host::kMaxNameLen] = {};
    Animation animId = Animation::None;
    size_t paramCount = 0;
    extension_host::ParamInfo paramInfo[RGBX_MAX_PARAMS] = {};
    uint32_t paramValues[RGBX_MAX_PARAMS] = {};
    char paramStrings[RGBX_MAX_PARAMS][RGBX_PARAM_STRING_MAX] = {};
    /* Simulates extension_host::paramInfo() returning nullptr for an index
     * that IS within paramCount - the defensive branch read_param/write_param
     * take when info lookup fails despite a valid-looking ParamCtx. */
    bool hideParamInfo[RGBX_MAX_PARAMS] = {};
};

FakeSlot sFakeSlots[extension_host::kMaxExtensions];

void reset_fake_slot(size_t slot) {
    sFakeSlots[slot] = FakeSlot{};
    sFakeSlots[slot].animId =
        static_cast<Animation>(extension_host::kAnimationIdBase + slot);
}

void set_param(size_t slot, size_t idx, const char *name, rgbx_param_type type,
              uint32_t defaultValue, const char *defaultStr = nullptr) {
    FakeSlot &fs = sFakeSlots[slot];
    strncpy(fs.paramInfo[idx].name, name, extension_host::kMaxParamNameLen - 1);
    fs.paramInfo[idx].name[extension_host::kMaxParamNameLen - 1] = '\0';
    fs.paramInfo[idx].type = type;
    fs.paramInfo[idx].defaultValue = defaultValue;
    fs.paramInfo[idx].stringSlot = extension_manifest::kNoStringSlot;
    fs.paramValues[idx] = defaultValue;
    if (defaultStr != nullptr) {
        strncpy(fs.paramStrings[idx], defaultStr, RGBX_PARAM_STRING_MAX - 1);
        fs.paramStrings[idx][RGBX_PARAM_STRING_MAX - 1] = '\0';
    }
}

}  // namespace

namespace extension_host {

bool isLoaded(size_t slot) {
    return slot < kMaxExtensions && sFakeSlots[slot].loaded;
}

bool isFaulted(size_t slot) {
    return slot < kMaxExtensions && sFakeSlots[slot].faulted;
}

const char *name(size_t slot) {
    return sFakeSlots[slot].name;
}

size_t paramCount(size_t slot) {
    return sFakeSlots[slot].paramCount;
}

const ParamInfo *paramInfo(size_t slot, size_t index) {
    if (index >= sFakeSlots[slot].paramCount || sFakeSlots[slot].hideParamInfo[index]) {
        return nullptr;
    }
    return &sFakeSlots[slot].paramInfo[index];
}

uint32_t paramValue(size_t slot, size_t index) {
    return sFakeSlots[slot].paramValues[index];
}

void setParamValue(size_t slot, size_t index, uint32_t value) {
    sFakeSlots[slot].paramValues[index] = value;
}

const char *paramString(size_t slot, size_t index) {
    return sFakeSlots[slot].paramStrings[index];
}

bool writeParamString(size_t slot, size_t index, size_t offset, const void *data, size_t len) {
    if (offset + len >= RGBX_PARAM_STRING_MAX) {
        return false;
    }
    char *dst = sFakeSlots[slot].paramStrings[index];
    memcpy(dst + offset, data, len);
    dst[offset + len] = '\0';
    return true;
}

Animation animationId(size_t slot) {
    return sFakeSlots[slot].animId;
}

}  // namespace extension_host

/* ---- Fakes standing in for pattern_controller.cpp ----------------------- */

namespace {
Animation gCurrentAnimation = Animation::None;
/* One-shot: simulates a synchronous refusal (e.g. getAnimation() failed
 * inside the real pattern_controller_change_to_animation) by NOT flipping
 * the registry's is-active state, mirroring what write_is_active checks
 * (bs->isActive == 0) to decide whether to reject the write. */
bool gRefuseNextChange = false;
}  // namespace

Animation pattern_controller_get_current_animation(void) {
    return gCurrentAnimation;
}

int pattern_controller_change_to_animation(Animation animation, bool persist) {
    (void)persist;
    if (gRefuseNextChange) {
        gRefuseNextChange = false;
        return 0;
    }
    animation_registry_set_is_active(gCurrentAnimation, false);
    gCurrentAnimation = animation;
    animation_registry_set_is_active(animation, true);
    return 0;
}

/* ---- animation_registry factory (never actually invoked: extension_bt.cpp
 * only drives is-active setters, never animation_registry_get()) ---------- */

namespace {

BaseAnimation *dummy_factory() {
    return nullptr;
}

/* ---- GATT attribute lookup helpers, using the real public API ---------- */

struct FindCtx {
    const bt_uuid_128 *wantSvcUuid;
    const bt_gatt_attr *found;
};

uint8_t primary_service_iter(const bt_gatt_attr *attr, uint16_t handle, void *user_data) {
    ARG_UNUSED(handle);
    auto *ctx = static_cast<FindCtx *>(user_data);
    const auto *svcUuid = static_cast<const bt_uuid *>(attr->user_data);
    if (svcUuid->type == BT_UUID_TYPE_128 &&
        memcmp(BT_UUID_128(svcUuid)->val, ctx->wantSvcUuid->val, sizeof(ctx->wantSvcUuid->val)) ==
            0) {
        ctx->found = attr;
        return BT_GATT_ITER_STOP;
    }
    return BT_GATT_ITER_CONTINUE;
}

/* Finds the primary-service attribute whose (128-bit) service UUID equals
 * `want` exactly - this doubles as the "generated UUID bytes equal the
 * BT_ANIMATION_SERVICE_UUID(id) macro expansion" assertion the issue calls
 * for: if extension_bt_register()'s runtime patch produced different bytes,
 * this lookup would find nothing. */
const bt_gatt_attr *find_primary_service(const bt_uuid_128 &want) {
    FindCtx ctx{&want, nullptr};
    bt_gatt_foreach_attr_type(0x0001, 0xffff, BT_UUID_GATT_PRIMARY, nullptr, 0,
                              primary_service_iter, &ctx);
    return ctx.found;
}

uint8_t single_match_iter(const bt_gatt_attr *attr, uint16_t handle, void *user_data) {
    ARG_UNUSED(handle);
    *static_cast<const bt_gatt_attr **>(user_data) = attr;
    return BT_GATT_ITER_STOP;
}

/* Scoped to [serviceStartHandle, serviceStartHandle+200) so a fixed-UUID
 * characteristic (Animation Name / Is Active / metadata, shared across every
 * registered extension service) resolves to THIS service's instance even if
 * another slot happens to be registered at the same time. 200 comfortably
 * covers one service's worst-case attr count (kMaxAttrs in extension_bt.cpp,
 * ~76 with RGBX_MAX_PARAMS=16). */
const bt_gatt_attr *find_char_value(uint16_t serviceStartHandle, const bt_uuid *uuid) {
    const bt_gatt_attr *found = nullptr;
    bt_gatt_foreach_attr_type(serviceStartHandle, serviceStartHandle + 200, uuid, nullptr, 1,
                              single_match_iter, &found);
    return found;
}

ssize_t do_read(const bt_gatt_attr *attr, void *buf, size_t bufLen) {
    return attr->read(nullptr, attr, buf, bufLen, 0);
}

ssize_t do_write(const bt_gatt_attr *attr, const void *data, size_t len, uint16_t offset = 0,
                 uint8_t flags = 0) {
    return attr->write(nullptr, attr, data, len, offset, flags);
}

}  // namespace

/* ---- Suite lifecycle ----------------------------------------------------- */

static void extension_bt_before(void *fixture) {
    ARG_UNUSED(fixture);
    animation_registry_reset();
    gCurrentAnimation = Animation::None;
    gRefuseNextChange = false;
}

ZTEST_SUITE(extension_bt, NULL, NULL, extension_bt_before, NULL, NULL);

/* ---- Tests ---------------------------------------------------------------- */

ZTEST(extension_bt, test_invalid_slot_arguments) {
    zassert_equal(extension_bt_register(extension_host::kMaxExtensions), -EINVAL);

    reset_fake_slot(9);
    sFakeSlots[9].loaded = false;
    zassert_equal(extension_bt_register(9), -EINVAL, "unloaded slot must be rejected");

    zassert_equal(extension_bt_bind_is_active(extension_host::kMaxExtensions), -EINVAL);
    zassert_equal(extension_bt_bind_is_active(9), -EINVAL, "unregistered slot must be rejected");

    /* Must not crash. */
    extension_bt_unregister(extension_host::kMaxExtensions);
    extension_bt_unregister(9);
}

ZTEST(extension_bt, test_register_uuid_and_is_active_lifecycle) {
    const size_t slot = 0;
    reset_fake_slot(slot);
    sFakeSlots[slot].loaded = true;
    strncpy(sFakeSlots[slot].name, "Test Ext", sizeof(sFakeSlots[slot].name) - 1);

    const uint16_t animId = extension_host::kAnimationIdBase + slot;
    const bt_uuid_128 expectedSvcUuid = BT_ANIMATION_SERVICE_UUID(animId);

    zassert_equal(extension_bt_register(slot), 0);
    /* Idempotent: registering an already-registered slot is a cheap no-op,
     * not a second attribute-table build. */
    zassert_equal(extension_bt_register(slot), 0);

    const bt_gatt_attr *svcAttr = find_primary_service(expectedSvcUuid);
    zassert_not_null(svcAttr, "extension service UUID does not match "
                              "BT_ANIMATION_SERVICE_UUID(id) expansion");

    char nameBuf[extension_host::kMaxNameLen] = {};
    const bt_gatt_attr *nameAttr =
        find_char_value(svcAttr->handle, &kAnimationNameCharacteristicUuid.uuid);
    zassert_not_null(nameAttr);
    ssize_t n = do_read(nameAttr, nameBuf, sizeof(nameBuf) - 1);
    zassert_true(n > 0);
    nameBuf[n] = '\0';
    zassert_str_equal(nameBuf, "Test Ext");

    const bt_gatt_attr *isActiveAttr =
        find_char_value(svcAttr->handle, &kIsActiveCharacteristicUuid.uuid);
    zassert_not_null(isActiveAttr);
    uint8_t isActive = 0xFF;
    zassert_equal(do_read(isActiveAttr, &isActive, sizeof(isActive)), sizeof(isActive));
    zassert_equal(isActive, 0);

    /* Bulk metadata characteristic (issue #90): Animation Name + Is Active,
     * no params on this slot => 2 entries. */
    const bt_gatt_attr *metaAttr =
        find_char_value(svcAttr->handle, &kMetadataCharacteristicUuid.uuid);
    zassert_not_null(metaAttr);
    uint8_t metaBuf[extension_metadata_blob::kMaxBlobSize] = {};
    ssize_t metaLen = do_read(metaAttr, metaBuf, sizeof(metaBuf));
    zassert_true(metaLen >= 2);
    zassert_equal(metaBuf[0], extension_metadata_blob::kVersion);
    zassert_equal(metaBuf[0], kMetadataBlobVersion);
    zassert_equal(metaBuf[1], 2, "expected Animation Name + Is Active metadata entries");

    /* Wire the is-active mirror into the registry, same order the real boot
     * path uses (proxy registration before the bind call). */
    zassert_equal(animation_registry_register(sFakeSlots[slot].animId, dummy_factory), 0);
    zassert_equal(extension_bt_bind_is_active(slot), 0);

    /* Activate. */
    uint8_t one = 1;
    zassert_equal(do_write(isActiveAttr, &one, sizeof(one)), sizeof(one));
    isActive = 0xFF;
    zassert_equal(do_read(isActiveAttr, &isActive, sizeof(isActive)), sizeof(isActive));
    zassert_equal(isActive, 1);
    zassert_equal(pattern_controller_get_current_animation(), sFakeSlots[slot].animId);

    /* Invalid write length. */
    uint8_t two[2] = {1, 1};
    zassert_equal(do_write(isActiveAttr, two, sizeof(two)), BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));

    /* Deactivate (current animation matches this slot's id). */
    uint8_t zero = 0;
    zassert_equal(do_write(isActiveAttr, &zero, sizeof(zero)), sizeof(zero));
    isActive = 0xFF;
    zassert_equal(do_read(isActiveAttr, &isActive, sizeof(isActive)), sizeof(isActive));
    zassert_equal(isActive, 0);
    zassert_equal(pattern_controller_get_current_animation(), Animation::None);

    /* A faulted slot rejects activation outright. */
    sFakeSlots[slot].faulted = true;
    zassert_equal(do_write(isActiveAttr, &one, sizeof(one)),
                 BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED));
    sFakeSlots[slot].faulted = false;

    /* A synchronously-refused switch (bs->isActive stays 0) is also rejected,
     * so the app's optimistic toggle reverts instead of sticking on. */
    gRefuseNextChange = true;
    zassert_equal(do_write(isActiveAttr, &one, sizeof(one)),
                 BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED));

    extension_bt_unregister(slot);
    zassert_is_null(find_primary_service(expectedSvcUuid));
    /* Unregistering twice must not crash. */
    extension_bt_unregister(slot);
}

ZTEST(extension_bt, test_param_characteristics) {
    const size_t slot = 1;
    reset_fake_slot(slot);
    sFakeSlots[slot].loaded = true;
    strncpy(sFakeSlots[slot].name, "Params", sizeof(sFakeSlots[slot].name) - 1);
    sFakeSlots[slot].paramCount = 4;
    set_param(slot, 0, "Speed", RGBX_PARAM_UINT32, 42);
    set_param(slot, 1, "Hue", RGBX_PARAM_COLOR, 0x00112233);
    set_param(slot, 2, "Enabled", RGBX_PARAM_BOOL, 0);
    set_param(slot, 3, "Label", RGBX_PARAM_STRING, 0, "hi");

    const uint16_t animId = extension_host::kAnimationIdBase + slot;
    const bt_uuid_128 svcUuid = BT_ANIMATION_SERVICE_UUID(animId);

    zassert_equal(extension_bt_register(slot), 0);
    const bt_gatt_attr *svcAttr = find_primary_service(svcUuid);
    zassert_not_null(svcAttr);

    bt_uuid_128 paramUuid[4];
    const bt_gatt_attr *paramAttr[4];
    for (uint16_t i = 0; i < 4; i++) {
        paramUuid[i] = composeAutoCharacteristicUuid(svcUuid, static_cast<uint16_t>(i + 1));
        paramAttr[i] = find_char_value(svcAttr->handle, &paramUuid[i].uuid);
        zassert_not_null(paramAttr[i], "missing param characteristic %u", i);
    }

    /* UINT32 "Speed". */
    uint32_t u32 = 0;
    zassert_equal(do_read(paramAttr[0], &u32, sizeof(u32)), sizeof(u32));
    zassert_equal(u32, 42);
    uint32_t newSpeed = 100;
    zassert_equal(do_write(paramAttr[0], &newSpeed, sizeof(newSpeed)), sizeof(newSpeed));
    zassert_equal(extension_host::paramValue(slot, 0), 100);
    zassert_equal(do_read(paramAttr[0], &u32, sizeof(u32)), sizeof(u32));
    zassert_equal(u32, 100);
    uint8_t shortBuf[2] = {};
    zassert_equal(do_write(paramAttr[0], shortBuf, sizeof(shortBuf)),
                 BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));

    /* COLOR "Hue" - same wire shape as UINT32. */
    uint32_t color = 0;
    zassert_equal(do_read(paramAttr[1], &color, sizeof(color)), sizeof(color));
    zassert_equal(color, 0x00112233u);

    /* BOOL "Enabled". */
    uint8_t boolVal = 0xFF;
    zassert_equal(do_read(paramAttr[2], &boolVal, sizeof(boolVal)), sizeof(boolVal));
    zassert_equal(boolVal, 0);
    uint8_t enable = 1;
    zassert_equal(do_write(paramAttr[2], &enable, sizeof(enable)), sizeof(enable));
    zassert_equal(extension_host::paramValue(slot, 2), 1);
    zassert_equal(do_write(paramAttr[2], shortBuf, sizeof(shortBuf)),
                 BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));

    /* STRING "Label". */
    char strBuf[RGBX_PARAM_STRING_MAX] = {};
    ssize_t n = do_read(paramAttr[3], strBuf, sizeof(strBuf) - 1);
    zassert_true(n >= 0);
    strBuf[n] = '\0';
    zassert_str_equal(strBuf, "hi");
    /* Prepare-phase write is accepted with no effect. */
    zassert_equal(do_write(paramAttr[3], "xx", 2, 0, BT_GATT_WRITE_FLAG_PREPARE), 0);
    zassert_str_equal(extension_host::paramString(slot, 3), "hi");
    /* Real write. */
    zassert_equal(do_write(paramAttr[3], "hello", 5), 5);
    zassert_str_equal(extension_host::paramString(slot, 3), "hello");
    /* offset+len overflowing the string storage is rejected; a nonzero
     * offset reports INVALID_OFFSET specifically, and a zero offset with an
     * overlong len alone reports INVALID_ATTRIBUTE_LEN instead. */
    char pad[5] = {'a', 'a', 'a', 'a', 'a'};
    zassert_equal(do_write(paramAttr[3], pad, sizeof(pad), 30),
                 BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
    char longPad[RGBX_PARAM_STRING_MAX];
    memset(longPad, 'a', sizeof(longPad));
    zassert_equal(do_write(paramAttr[3], longPad, sizeof(longPad), 0),
                 BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));

    /* Defensive branch: paramInfo() unexpectedly returns nullptr for an
     * in-range index. */
    sFakeSlots[slot].hideParamInfo[0] = true;
    zassert_equal(do_read(paramAttr[0], &u32, sizeof(u32)), BT_GATT_ERR(BT_ATT_ERR_UNLIKELY));
    zassert_equal(do_write(paramAttr[0], &newSpeed, sizeof(newSpeed)),
                 BT_GATT_ERR(BT_ATT_ERR_UNLIKELY));
    sFakeSlots[slot].hideParamInfo[0] = false;

    extension_bt_unregister(slot);
}
