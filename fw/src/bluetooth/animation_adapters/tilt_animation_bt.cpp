#include <animations/animation_is_active_binding.h>
#include <animations/tilt_animation.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

constexpr bt_uuid_128 kTiltConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::Tilt));

BtGattPrimaryService<kTiltConfigServiceUuid> tiltPrimaryService;

using TiltIsActiveCharacteristic = IsActiveCharacteristic<Animation::Tilt>;
TiltIsActiveCharacteristic tiltIsActive;

constexpr BtGattString<24> kTiltAnimationName = makeBtGattString<24>("Tilt");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kTiltAnimationName>
    tiltAnimationName;

BtGattServer tiltConfigServer(tiltPrimaryService, tiltIsActive, tiltAnimationName);
BT_GATT_SERVER_REGISTER(tiltConfigServerStatic, tiltConfigServer);

using TiltAnimationIsActive = AnimationIsActiveBinding<Animation::Tilt>;

static void tilt_set_is_active(bool active) {
    tiltIsActive.setActive(active);
}

struct TiltIsActiveBindingRegistrar {
    TiltIsActiveBindingRegistrar() {
        TiltAnimationIsActive::registerSetter(tilt_set_is_active);
    }
};

[[maybe_unused]] TiltIsActiveBindingRegistrar sTiltIsActiveBindingRegistrar;

void tilt_animation_bind_default_bt_dependencies() {
    /* TiltAnimation has no BT-backed parameters — nothing to inject. */
}
