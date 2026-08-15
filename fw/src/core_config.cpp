#include <animations/active_animation_binding.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>
#include <core_config.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(core_config, LOG_LEVEL_INF);

constexpr bt_uuid_128 kCoreConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, CoreConfig::kServiceIdNum, 0x56789abc0000));

BtGattPrimaryService<kCoreConfigServiceUuid> coreConfigPrimaryService;

// ---------------------------------------------------------------------------
// THE NOTIFICATION-BUDGET RULE (this comment is the reference other services
// point at; keep it here, don't duplicate it)
//
// Android caps GATT notification registrations at ~15 per app
// (BTA_GATTC_NOTIF_REG_MAX) and SILENTLY drops the overflow — the CCC write
// still succeeds, so firmware notifies into a void. That is what broke every
// companion-app firmware update in fw-v2.1.0 (diagnosis: /debug-ble section 4a).
//
// The cap is on CONCURRENT REGISTRATIONS, not on how many characteristics are
// declared notifiable. So the division of responsibility is:
//
//   FIRMWARE declares Notify=true wherever a genuine DEVICE-SIDE push exists
//   (a shell command, a button, a sensor/charger thread, a fault) — and
//   Notify=false where only the app ever changes the value, because there the
//   app's own read-back-after-write already tells it what the device stored.
//
//   THE APP decides WHEN to register: a tiny always-on set, plus subscriptions
//   scoped to the focused screen or to the active animation
//   (app/hooks/use-scoped-characteristic-monitors.ts).
//
// So do NOT reach for Notify=false to save budget on a value the device really
// does push — scope it app-side instead. Reserve Notify=false for
// app-written-only values.
// ---------------------------------------------------------------------------

// These four are app-written tunables with no device-side writer, so Notify=false
// is correct on its own terms: the clamp write-back in the getters below reaches
// the app via its read-back-after-write on non-notifiable characteristics.
BtGattPersistentCharacteristic<"core/brightness", "Brightness (0-1000)", false, uint32_t, 20>
    coreBrightness;
BtGattPersistentCharacteristic<"core/display_thread_rate_ms", "Display Thread Rate * 1000 (ms)",
                                false, uint32_t, 33300>
    coreDisplayThreadRateMs;
// Default matches the display rate: rendering faster than the display push just
// throws frames away (issue #376) — getRenderRateMs() enforces this as a floor.
BtGattPersistentCharacteristic<"core/render_thread_rate_ms", "Render Thread Rate * 1000 (ms)",
                                false, uint32_t, 33300>
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
// per-animation Is Active characteristics), NOT persisted — and as of issue #311
// nothing else persists it either: there is no boot restore, the device always
// comes up on the default animation. Value: uint32
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
    const uint32_t displayRateUint = coreDisplayThreadRateMs;

    // A render interval shorter than the display interval produces frames the
    // display thread never samples (issue #376), so floor it at the display
    // interval. Rendering SLOWER than the display stays allowed. The write-back
    // is RAM-only (operator= does not persist — see persistent_characteristic.h):
    // deliberate settings migration in the AudioConfig::getTargetHigh() idiom, so
    // boards that persisted the old 11100 default are floored on the first render
    // tick of every boot, and the app sees the corrected value on its read-back
    // (these characteristics are non-notifiable, see the comment above).
    if (renderRateUint < displayRateUint) {
        renderRateUint = displayRateUint;
        coreRenderThreadRateMs = renderRateUint;
    }

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
