#include <animations/animation_is_active_binding.h>
#include <animations/pulse_animation.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>

constexpr bt_uuid_128 kPulseConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::Pulse));

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

BtGattServer pulseConfigServer(pulsePrimaryService, pulseColor, pulsePeriodMs, pulseIsActive,
                                pulseAnimationName);
BT_GATT_SERVER_REGISTER(pulseConfigServerStatic, pulseConfigServer);

namespace {
class PulseColorSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return static_cast<BtGattColor>(pulseColor); }
};

class PulsePeriodMsSource : public AnimationUint32ParameterSource {
   public:
    uint32_t get() const override { return pulsePeriodMs; }
};

PulseColorSource sDefaultColorSource;
PulsePeriodMsSource sDefaultPeriodMsSource;
PulseAnimationDependencies sDefaultDeps(sDefaultColorSource, sDefaultPeriodMsSource);
}  // namespace

using PulseAnimationIsActive = AnimationIsActiveBinding<Animation::Pulse>;

static void pulse_set_is_active(bool active) {
    pulseIsActive.setActive(active);
}

struct PulseIsActiveBindingRegistrar {
    PulseIsActiveBindingRegistrar() {
        PulseAnimationIsActive::registerSetter(pulse_set_is_active);
    }
};

[[maybe_unused]] PulseIsActiveBindingRegistrar sPulseIsActiveBindingRegistrar;

void pulse_animation_bind_default_dependencies() {
    PulseAnimation::getInstance()->setDependencies(sDefaultDeps);
}
