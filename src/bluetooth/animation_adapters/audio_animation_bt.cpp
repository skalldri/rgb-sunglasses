#include <animations/audio_animation.h>
#include <animations/animation_is_active_binding.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

#include <zephyr/bluetooth/uuid.h>

constexpr bt_uuid_128 kAudioConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x0200, 0x56789abd0005));

BtGattPrimaryService<kAudioConfigServiceUuid> audioPrimaryService;
BtGattAutoReadWriteCharacteristic<"Mode", uint32_t, 0> audioMode;
BtGattAutoReadWriteCharacteristic<"Color", BtGattColor, BtGattColor{0xFFFFFFFF}> audioColor;

using AudioIsActiveCharacteristic = IsActiveCharacteristic<Animation::Audio>;
AudioIsActiveCharacteristic audioIsActive;

BtGattServer audioConfigServer(
    audioPrimaryService,
    audioMode,
    audioColor,
    audioIsActive);
BT_GATT_SERVER_REGISTER(audioConfigServerStatic, audioConfigServer);

namespace
{
    class AudioModeSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return audioMode;
        }
    };

    class AudioColorSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return static_cast<BtGattColor>(audioColor);
        }
    };

    AudioModeSource sDefaultModeSource;
    AudioColorSource sDefaultColorSource;
}

using AudioAnimationIsActive = AnimationIsActiveBinding<Animation::Audio>;

static void audio_set_is_active(bool active)
{
    audioIsActive.setActive(active);
}

struct AudioIsActiveBindingRegistrar
{
    AudioIsActiveBindingRegistrar()
    {
        AudioAnimationIsActive::registerSetter(audio_set_is_active);
    }
};

[[maybe_unused]] AudioIsActiveBindingRegistrar sAudioIsActiveBindingRegistrar;

void audio_animation_bind_default_bt_dependencies()
{
    AudioAnimation::getInstance()->setBtParameters(sDefaultModeSource, sDefaultColorSource);
}
