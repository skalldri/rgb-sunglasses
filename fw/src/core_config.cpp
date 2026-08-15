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

// One shared default for both thread rates (ms x 1000 = 33.3 ms, ~30 Hz): the
// render default deliberately equals the display default, since rendering faster
// than the display push just throws frames away (issue #376) — getRenderRateMs()
// also enforces this as a runtime floor, whatever the persisted values say.
//
// Known cosmetic cost of 1:1 (PR #378 review): the two loops free-run with no
// buffer-swap handshake, and each paces itself with a whole-ms k_msleep, so
// their ~33 ms periods differ slightly and the phase slips — roughly once per
// tens of seconds the display samples the same framebuffer twice (or skips
// one), visible as a single 2 px jump or held frame on a 1 px/step scroll.
// The old 3:1 ratio hid this by always having a fresh frame ready, at 3x the
// render cost. A producer/consumer handshake on the swap would remove it
// entirely — tracked in issue #379.
constexpr uint32_t kDefaultThreadRateMsX1000 = 33300;

// These four are app-written tunables with no device-side writer, so Notify=false
// is correct on its own terms: the clamp write-back in the getters below reaches
// the app via its read-back-after-write on non-notifiable characteristics.
BtGattPersistentCharacteristic<"core/brightness", "Brightness (0-1000)", false, uint32_t, 20>
    coreBrightness;
BtGattPersistentCharacteristic<"core/display_thread_rate_ms", "Display Thread Rate * 1000 (ms)",
                                false, uint32_t, kDefaultThreadRateMsX1000>
    coreDisplayThreadRateMs;
BtGattPersistentCharacteristic<"core/render_thread_rate_ms", "Render Thread Rate * 1000 (ms)",
                                false, uint32_t, kDefaultThreadRateMsX1000>
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

    // Migration only: correct the exact pre-#376 default in RAM (operator= does
    // not persist — see persistent_characteristic.h; the AudioConfig::
    // getTargetHigh() idiom) so boards that persisted 11100 read back the new
    // default. Two guards keep it from stealing anything a user meant
    // (PR #378 review): exact match — any other value is a user's own setting —
    // AND only when 11100 sits below the display interval, i.e. it is actually
    // the stale faster-than-display default. A deliberate 90 Hz setup
    // (display = render = 11100) is left exactly as configured.
    if (renderRateUint == 11100 && renderRateUint < displayRateUint) {
        renderRateUint = kDefaultThreadRateMsX1000;
        coreRenderThreadRateMs = renderRateUint;
    }

    // A render interval shorter than the display interval produces frames the
    // display thread never samples (issue #376), so floor it at the display
    // interval — at point of use, WITHOUT writing back. A write-back here would
    // destroy the user's render rate whenever the display rate is temporarily
    // raised (and any later debounced save would persist the destruction);
    // clamping only the returned value makes the floor fully reversible.
    // Rendering SLOWER than the display stays allowed.
    if (renderRateUint < displayRateUint) {
        renderRateUint = displayRateUint;
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
