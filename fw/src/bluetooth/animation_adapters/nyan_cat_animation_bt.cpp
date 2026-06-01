#include <animations/nyan_cat_animation.h>
#include <animations/animation_is_active_binding.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

constexpr bt_uuid_128 kNyanCatConfigServiceUuid = BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::NyanCat));

BtGattPrimaryService<kNyanCatConfigServiceUuid> nyanCatPrimaryService;

using NyanCatIsActiveCharacteristic = IsActiveCharacteristic<Animation::NyanCat>;
NyanCatIsActiveCharacteristic nyanCatIsActive;

BtGattServer nyanCatConfigServer(
    nyanCatPrimaryService,
    nyanCatIsActive);
BT_GATT_SERVER_REGISTER(nyanCatConfigServerStatic, nyanCatConfigServer);

using NyanCatAnimationIsActive = AnimationIsActiveBinding<Animation::NyanCat>;

static void nyan_cat_set_is_active(bool active)
{
    nyanCatIsActive.setActive(active);
}

struct NyanCatIsActiveBindingRegistrar
{
    NyanCatIsActiveBindingRegistrar()
    {
        NyanCatAnimationIsActive::registerSetter(nyan_cat_set_is_active);
    }
};

[[maybe_unused]] NyanCatIsActiveBindingRegistrar sNyanCatIsActiveBindingRegistrar;

void nyan_cat_animation_bind_default_bt_dependencies()
{
    // No BT-injected parameters for NyanCatAnimation; playback is file-driven.
}
