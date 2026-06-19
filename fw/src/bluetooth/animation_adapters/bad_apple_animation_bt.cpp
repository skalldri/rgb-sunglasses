#include <animations/bad_apple_animation.h>
#include <animations/animation_is_active_binding.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

constexpr bt_uuid_128 kBadAppleConfigServiceUuid = BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::BadApple));

BtGattPrimaryService<kBadAppleConfigServiceUuid> badApplePrimaryService;

using BadAppleIsActiveCharacteristic = IsActiveCharacteristic<Animation::BadApple>;
BadAppleIsActiveCharacteristic badAppleIsActive;

constexpr BtGattString<24> kBadAppleAnimationName = makeBtGattString<24>("Bad Apple");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kBadAppleAnimationName>
    badAppleAnimationName;

BtGattServer badAppleConfigServer(
    badApplePrimaryService,
    badAppleIsActive,
    badAppleAnimationName);
BT_GATT_SERVER_REGISTER(badAppleConfigServerStatic, badAppleConfigServer);

using BadAppleAnimationIsActive = AnimationIsActiveBinding<Animation::BadApple>;

static void bad_apple_set_is_active(bool active)
{
    badAppleIsActive.setActive(active);
}

struct BadAppleIsActiveBindingRegistrar
{
    BadAppleIsActiveBindingRegistrar()
    {
        BadAppleAnimationIsActive::registerSetter(bad_apple_set_is_active);
    }
};

[[maybe_unused]] BadAppleIsActiveBindingRegistrar sBadAppleIsActiveBindingRegistrar;

void bad_apple_animation_bind_default_bt_dependencies()
{
    // No BT-injected parameters for BadAppleAnimation; playback is file-driven.
}
