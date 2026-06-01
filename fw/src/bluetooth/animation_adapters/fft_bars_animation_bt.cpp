#include <animations/animation_is_active_binding.h>
#include <animations/fft_bars_animation.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

constexpr bt_uuid_128 kFftBarsConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::FftBars));

BtGattPrimaryService<kFftBarsConfigServiceUuid> fftBarsPrimaryService;

using FftBarsIsActiveCharacteristic = IsActiveCharacteristic<Animation::FftBars>;
FftBarsIsActiveCharacteristic fftBarsIsActive;

BtGattServer fftBarsConfigServer(fftBarsPrimaryService, fftBarsIsActive);
BT_GATT_SERVER_REGISTER(fftBarsConfigServerStatic, fftBarsConfigServer);

using FftBarsAnimationIsActive = AnimationIsActiveBinding<Animation::FftBars>;

static void fft_bars_set_is_active(bool active) {
    fftBarsIsActive.setActive(active);
}

struct FftBarsIsActiveBindingRegistrar {
    FftBarsIsActiveBindingRegistrar() {
        FftBarsAnimationIsActive::registerSetter(fft_bars_set_is_active);
    }
};

[[maybe_unused]] FftBarsIsActiveBindingRegistrar sFftBarsIsActiveBindingRegistrar;

void fft_bars_animation_bind_default_bt_dependencies() {
    /* No BT-injected parameters for FftBarsAnimation; colours are fixed. */
}
