#include <animations/animation_is_active_binding.h>
#include <animations/animation_shuffle_include_binding.h>
#include <animations/color_mode_source.h>
#include <animations/pulse_animation.h>
#include <zephyr/random/random.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/animation_shuffle_include_characteristic.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>

constexpr bt_uuid_128 kPulseConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::Pulse));

// Breathing and beat sync are mutually exclusive (issue #148): turning one on turns the
// other off. Declared here so the characteristics below can name them; defined after both,
// since each hook has to reach its sibling.
static void pulse_breathing_written(const bool &enabled);
static void pulse_beat_sync_written(const bool &enabled);

BtGattPrimaryService<kPulseConfigServiceUuid> pulsePrimaryService;
BtGattPersistentCharacteristic<"pulse/color", "Color", false, BtGattColor, BtGattColor{0xFFFFFFFF}>
    pulseColor;
BtGattPersistentCharacteristic<"pulse/period_ms", "Period Ms", false, uint32_t, 2000> pulsePeriodMs;

using PulseIsActiveCharacteristic = IsActiveCharacteristic<Animation::Pulse>;
PulseIsActiveCharacteristic pulseIsActive;

constexpr BtGattString<24> kPulseAnimationName = makeBtGattString<24>("Pulse");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kPulseAnimationName>
    pulseAnimationName;

// APPEND-ONLY: new providers go after every existing one (fixed UUID, so auto-UUID
// positions don't shift either way — but bonded phones cache handles per table).
ShuffleIncludeCharacteristic<"pulse/shuffle"> pulseShuffleInclude;

// Notify is ON for both, unlike every other app-written tunable here. Writing one clears
// the other device-side, and the app has no way to predict that from its own write alone;
// the animation-detail screen scope-subscribes to exactly these (it skips Is Active and
// Shuffle Include) so the losing switch flips in the UI as it flips on the device, and the
// two slots are only registered while that one screen is focused — well inside Android's
// ~15-slot budget (see fw/src/core_config.cpp).
BtGattPersistentCharacteristic<"pulse/breathing", "Breathing", true, bool, true,
                               &pulse_breathing_written>
    pulseBreathing;
BtGattPersistentCharacteristic<"pulse/beat_sync", "Beat Sync", true, bool, false,
                               &pulse_beat_sync_written>
    pulseBeatSync;

BtGattServer pulseConfigServer(pulsePrimaryService, pulseColor, pulsePeriodMs, pulseIsActive,
                                pulseAnimationName, pulseShuffleInclude, pulseBreathing,
                                pulseBeatSync);
BT_GATT_SERVER_REGISTER(pulseConfigServerStatic, pulseConfigServer);

// Only ever clears the sibling, never sets it, so the two hooks cannot ping-pong even
// before accounting for operator= bypassing onWrite: a clear passes `false` and returns
// immediately. Turning a toggle OFF leaves the other alone — both off is the valid
// "solid color / flashlight" state this issue asked for.
static void pulse_breathing_written(const bool &enabled) {
    if (!enabled || !pulseBeatSync.value()) {
        return;
    }
    pulseBeatSync = false;
    pulseBeatSync.mark_dirty();
    persistent_value_store::request_save();
}

static void pulse_beat_sync_written(const bool &enabled) {
    if (!enabled || !pulseBreathing.value()) {
        return;
    }
    pulseBreathing = false;
    pulseBreathing.mark_dirty();
    persistent_value_store::request_save();
}

namespace {
class PulseColorSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return static_cast<BtGattColor>(pulseColor); }
};

class PulsePeriodMsSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return pulsePeriodMs; }
};

class PulseBreathingSource : public AnimationBoolParameterSource {
   public:
    bool get() const override { return pulseBreathing.value(); }
};

class PulseBeatSyncSource : public AnimationBoolParameterSource {
   public:
    bool get() const override { return pulseBeatSync.value(); }
};

PulseColorSource sDefaultColorSource;
// Resolves the color value's mode byte (issue #259) so the animation always sees
// an effective 0x00RRGGBB through the same interface.
ColorModeSource sPulseColorMode(sDefaultColorSource, sys_rand32_get, k_uptime_get);
PulsePeriodMsSource sDefaultPeriodMsSource;
PulseBreathingSource sDefaultBreathingSource;
PulseBeatSyncSource sDefaultBeatSyncSource;
PulseAnimationDependencies sDefaultDeps(sPulseColorMode, sDefaultPeriodMsSource,
                                        sDefaultBreathingSource, sDefaultBeatSyncSource);
}  // namespace

using PulseAnimationIsActive = AnimationIsActiveBinding<Animation::Pulse>;

static void pulse_set_is_active(bool active) {
    if (active) {
        // Fires for every activation source (BLE write, shell, boot restore,
        // shuffle) — arms the RandomOnActivate re-roll / mode-state reset.
        sPulseColorMode.notifyActivated();
    }
    pulseIsActive.setActive(active);
}

static bool pulse_shuffle_included() {
    return pulseShuffleInclude.value();
}

struct PulseIsActiveBindingRegistrar {
    PulseIsActiveBindingRegistrar() {
        PulseAnimationIsActive::registerSetter(pulse_set_is_active);
        AnimationShuffleIncludeBinding<Animation::Pulse>::registerGetter(pulse_shuffle_included);
    }
};

[[maybe_unused]] PulseIsActiveBindingRegistrar sPulseIsActiveBindingRegistrar;

void pulse_animation_bind_default_dependencies() {
    PulseAnimation::getInstance()->setDependencies(sDefaultDeps);
}
