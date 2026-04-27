#include <animations/zigzag_animation.h>
#include <animations/animation_is_active_binding.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

#include <zephyr/bluetooth/uuid.h>

constexpr bt_uuid_128 kZigZagConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x0200, 0x56789abd0000));

BtGattPrimaryService<kZigZagConfigServiceUuid> zigzagPrimaryService;
BtGattAutoReadWriteCharacteristic<"Step Time Ms", uint32_t, 200> zigzagStepTimeMs;
BtGattAutoReadWriteCharacteristic<"Color", BtGattColor, BtGattColor{0xFFFFFFFF}> zigzagColor;

using ZigZagIsActiveCharacteristic = IsActiveCharacteristic<Animation::ZigZag>;
ZigZagIsActiveCharacteristic zigzagIsActive;

BtGattServer zigzagConfigServer(
    zigzagPrimaryService,
    zigzagStepTimeMs,
    zigzagColor,
    zigzagIsActive);
BT_GATT_SERVER_REGISTER(zigzagConfigServerStatic, zigzagConfigServer);

namespace
{
    class ZigZagStepTimeSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return zigzagStepTimeMs;
        }
    };

    class ZigZagColorSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return static_cast<BtGattColor>(zigzagColor);
        }
    };

    ZigZagStepTimeSource sDefaultStepTimeSource;
    ZigZagColorSource sDefaultColorSource;
    ZigZagAnimationDependencies sDefaultDeps(sDefaultStepTimeSource, sDefaultColorSource);
}

using ZigZagAnimationIsActive = AnimationIsActiveBinding<Animation::ZigZag>;

static void zigzag_set_is_active(bool active)
{
    zigzagIsActive.setActive(active);
}

struct ZigZagIsActiveBindingRegistrar
{
    ZigZagIsActiveBindingRegistrar()
    {
        ZigZagAnimationIsActive::registerSetter(zigzag_set_is_active);
    }
};

[[maybe_unused]] ZigZagIsActiveBindingRegistrar sZigZagIsActiveBindingRegistrar;

void zigzag_animation_bind_default_dependencies()
{
    ZigZagAnimation::getInstance()->setDependencies(sDefaultDeps);
}
