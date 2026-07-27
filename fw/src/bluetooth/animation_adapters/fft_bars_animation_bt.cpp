#include <animations/animation_is_active_binding.h>
#include <animations/animation_shuffle_include_binding.h>
#include <animations/fft_bars_animation.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/animation_shuffle_include_characteristic.h>
#include <bluetooth/bt_service_cpp.h>
#include <sound/audio_config.h>

constexpr bt_uuid_128 kFftBarsConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::FftBars));

BtGattPrimaryService<kFftBarsConfigServiceUuid> fftBarsPrimaryService;

using FftBarsIsActiveCharacteristic = IsActiveCharacteristic<Animation::FftBars>;
FftBarsIsActiveCharacteristic fftBarsIsActive;

constexpr BtGattString<24> kFftBarsAnimationName = makeBtGattString<24>("FFT Bars");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kFftBarsAnimationName>
    fftBarsAnimationName;

// APPEND-ONLY: new providers go after every existing one (fixed UUID, so auto-UUID
// positions don't shift either way — but bonded phones cache handles per table).
ShuffleIncludeCharacteristic<"fft_bars/shuffle"> fftBarsShuffleInclude;

BtGattServer fftBarsConfigServer(fftBarsPrimaryService, fftBarsIsActive, fftBarsAnimationName,
                                 fftBarsShuffleInclude);
BT_GATT_SERVER_REGISTER(fftBarsConfigServerStatic, fftBarsConfigServer);

using FftBarsAnimationIsActive = AnimationIsActiveBinding<Animation::FftBars>;

static void fft_bars_set_is_active(bool active) {
    fftBarsIsActive.setActive(active);
}

static bool fft_bars_shuffle_included() {
    return fftBarsShuffleInclude.value();
}

struct FftBarsIsActiveBindingRegistrar {
    FftBarsIsActiveBindingRegistrar() {
        FftBarsAnimationIsActive::registerSetter(fft_bars_set_is_active);
        AnimationShuffleIncludeBinding<Animation::FftBars>::registerGetter(
            fft_bars_shuffle_included);
    }
};

[[maybe_unused]] FftBarsIsActiveBindingRegistrar sFftBarsIsActiveBindingRegistrar;

void fft_bars_animation_bind_default_bt_dependencies() {
    FftBarsAnimation::getInstance()->setConfigSource(AudioConfig::getInstance());
}
