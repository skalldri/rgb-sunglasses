#include <animations/zigzag_animation.h>
#include <animations/animation_is_active_binding.h>
#include <animations/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

#include <zephyr/bluetooth/uuid.h>

constexpr bt_uuid_128 kZigZagConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x0200, 0x56789abd0000));

BtGattPrimaryService<kZigZagConfigServiceUuid> zigzagPrimaryService;
BtGattAutoReadWriteNotifyCharacteristic<"Step Time Ms", uint32_t, 200> zigzagStepTimeMs;
BtGattAutoReadWriteNotifyCharacteristic<"Color", BtGattColor, BtGattColor{0xFFFFFFFF}> zigzagColor;

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

// All services implement the "IsActive" service, so declare relevant BT GATT glue logic
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

void ZigZagAnimation::setDependencies(const ZigZagAnimationDependencies &deps)
{
    deps_ = &deps;
}

void ZigZagAnimation::init()
{
    currentCycleTimeMs = 0;
    currentIndex = 0;
}

void ZigZagAnimation::tick(const LedConfig *config, const size_t timeSinceLastTickMs, const size_t bufferId)
{
    if (!deps_)
    {
        setDependencies(sDefaultDeps);
    }

    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > deps_->stepTimeMs.get())
    {
        currentCycleTimeMs = 0;
        currentIndex++;
    }

    if (currentIndex >= (config->displayWidth * config->displayHeight))
    {
        currentIndex = 0;
    }

    // Turn off all LEDs
    for (size_t x = 0; x < config->displayWidth; x++)
    {
        for (size_t y = 0; y < config->displayHeight; y++)
        {
            pattern_controller_set_pixel_in_framebuffer(config, x, y, bufferId, 0, 0, 0);
        }
    }

    size_t y = currentIndex / config->displayWidth;
    size_t x = currentIndex % config->displayWidth;

    // Turn on just one

    uint32_t color = deps_->color.get();
    uint8_t red = (color >> 16) & 0xFF;
    uint8_t green = (color >> 8) & 0xFF;
    uint8_t blue = (color >> 0) & 0xFF;
    pattern_controller_set_pixel_in_framebuffer(config, x, y, bufferId, red, green, blue);
}