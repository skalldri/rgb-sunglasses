#include <animations/fft_bars_animation.h>
#include <animations/animation_is_active_binding.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

#include <zephyr/bluetooth/uuid.h>

constexpr bt_uuid_128 kFftBarsConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x0200, 0x56789abd0007));

BtGattPrimaryService<kFftBarsConfigServiceUuid> fftBarsPrimaryService;
BtGattAutoReadWriteCharacteristic<"Color", BtGattColor, BtGattColor{0xFF0000FF}> fftBarsColor;

using FftBarsIsActiveCharacteristic = IsActiveCharacteristic<Animation::FftBars>;
FftBarsIsActiveCharacteristic fftBarsIsActive;

BtGattServer fftBarsConfigServer(
    fftBarsPrimaryService,
    fftBarsColor,
    fftBarsIsActive);
BT_GATT_SERVER_REGISTER(fftBarsConfigServerStatic, fftBarsConfigServer);

namespace
{
    class FftBarsColorSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return static_cast<BtGattColor>(fftBarsColor);
        }
    };

    FftBarsColorSource sDefaultColorSource;
}

using FftBarsAnimationIsActive = AnimationIsActiveBinding<Animation::FftBars>;

static void fft_bars_set_is_active(bool active)
{
    fftBarsIsActive.setActive(active);
}

struct FftBarsIsActiveBindingRegistrar
{
    FftBarsIsActiveBindingRegistrar()
    {
        FftBarsAnimationIsActive::registerSetter(fft_bars_set_is_active);
    }
};

[[maybe_unused]] FftBarsIsActiveBindingRegistrar sFftBarsIsActiveBindingRegistrar;

void fft_bars_animation_bind_default_bt_dependencies()
{
    FftBarsAnimation::getInstance()->setColor(sDefaultColorSource);
}
