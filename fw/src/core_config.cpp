#include <animations/active_animation_binding.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>
#include <core_config.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(core_config, LOG_LEVEL_INF);

constexpr bt_uuid_128 kCoreConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, CoreConfig::kServiceIdNum, 0x56789abc0000));

BtGattPrimaryService<kCoreConfigServiceUuid> coreConfigPrimaryService;
// Notify=false on the four config values (they used to be true): Android caps GATT
// notification registrations at ~15 per app (BTA_GATTC_NOTIF_REG_MAX); these are
// app-written tunables, and the clamp write-back in the getters below reaches the
// app via its read-back-after-write on non-notifiable characteristics.
BtGattPersistentCharacteristic<"core/brightness", "Brightness (0-1000)", false, uint32_t, 20>
    coreBrightness;
BtGattPersistentCharacteristic<"core/display_thread_rate_ms", "Display Thread Rate * 1000 (ms)",
                                false, uint32_t, 33300>
    coreDisplayThreadRateMs;
BtGattPersistentCharacteristic<"core/render_thread_rate_ms", "Render Thread Rate * 1000 (ms)",
                                false, uint32_t, 11100>
    coreRenderThreadRateMs;
BtGattPersistentCharacteristic<"core/status_led_brightness", "Status LED Brightness (0-1000)",
                                false, uint32_t, 20>
    coreStatusLedBrightness;
// The issue-#121 shuffle characteristics (positions 4-6) moved to the dedicated Shuffle
// service (issue #243, src/bluetooth/shuffle_service.cpp). They were this table's LAST
// three entries, so the remaining positional UUIDs above are unchanged — but bonded
// phones cache GATT handles per table, so this restructure still needs a forget+re-pair
// on stacks that ignore Service Changed (the OxygenOS caveat, issue #115).

// Position 4 (auto-UUID suffix ...0004) — APPEND-ONLY, see the comment above.
// Read + notify, NOT writable (the app switches animations by writing the
// per-animation Is Active characteristics), NOT persisted (boot restore is
// pattern_controller.cpp's core/last_active_animation key). Value: uint32
// Animation id (animation_types.h); extensions are 0x40+slot
// (extension_limits.h); 0 = Animation::None. This is the ONE notification that
// replaces the per-animation Is Active notifies (see
// animation_is_active_characteristic.h) so device-side switches — shuffle hops,
// button presses, shell `anim set` — still reach the app within Android's
// registration budget. On an extension sandbox fault this KEEPS reporting the
// faulted extension's id: pattern_controller still renders that slot (the
// FAULT banner) until the user switches; the fault itself is pushed through
// the extension's own Is Active notify (extension_bt.cpp).
BtGattAutoReadNotifyCharacteristic<"Active Animation", uint32_t, 0> coreActiveAnimation;

BtGattServer coreConfigServer(coreConfigPrimaryService, coreBrightness, coreDisplayThreadRateMs,
                              coreRenderThreadRateMs, coreStatusLedBrightness,
                              coreActiveAnimation);
BT_GATT_SERVER_REGISTER(coreConfigServerStatic, coreConfigServer);

static void core_config_set_active_animation(Animation animation) {
    // operator= notifies subscribed centrals only on change; safe from the BT RX,
    // shell, and pattern-controller threads (same precedent as
    // battery_service_update / shuffle_service_set_enabled).
    coreActiveAnimation = static_cast<uint32_t>(animation);
}

// Plain static-ctor registrar, NOT SYS_INIT: C++ static constructors run before
// APPLICATION-level SYS_INIT (fw/CLAUDE.md "SYS_INIT ordering"), so the setter is
// in place before pattern_controller's boot restore fires the binding. Same idiom
// as the per-animation IsActiveBindingRegistrars in the animation adapters.
struct ActiveAnimationBindingRegistrar {
    ActiveAnimationBindingRegistrar() {
        ActiveAnimationBinding::registerSetter(core_config_set_active_animation);
    }
};
[[maybe_unused]] static ActiveAnimationBindingRegistrar sActiveAnimationBindingRegistrar;

float CoreConfig::getBrightnessFactor() {
    uint32_t brightnessUint = coreBrightness;

    // Clamp to sane values
    if (brightnessUint > 1000) {
        brightnessUint = 1000;
        coreBrightness = 1000;  // Clamp the BT variable as well
    }

    return ((float)brightnessUint) / 1000.0f;
}

float CoreConfig::getDisplayRateMs() {
    uint32_t displayRateUint = coreDisplayThreadRateMs;
    return ((float)displayRateUint) / 1000.0f;
}

float CoreConfig::getRenderRateMs() {
    uint32_t renderRateUint = coreRenderThreadRateMs;
    return ((float)renderRateUint) / 1000.0f;
}

float CoreConfig::getStatusLedBrightnessFactor() {
    uint32_t brightnessUint = coreStatusLedBrightness;

    // Clamp to sane values
    if (brightnessUint > 1000) {
        brightnessUint = 1000;
        coreStatusLedBrightness = 1000;
    }

    return ((float)brightnessUint) / 1000.0f;
}
