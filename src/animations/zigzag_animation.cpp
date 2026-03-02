#include <animations/zigzag_animation.h>
#include <animations/animation_is_active_binding.h>
#include <bluetooth/read_write_variable.h>

BT_SVC_UUID_DEFINE(ZigZagAnimation);

using StepTimeMs = BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(ZigZagAnimation, 0, uint32_t, 200);
using Color = BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(ZigZagAnimation, 1, Color, 0xFFFFFFFF);

namespace
{
    class ZigZagStepTimeSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return (uint32_t)StepTimeMs::getInstance();
        }
    };

    class ZigZagColorSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override
        {
            return (uint32_t)Color::getInstance();
        }
    };

    ZigZagStepTimeSource sDefaultStepTimeSource;
    ZigZagColorSource sDefaultColorSource;
    ZigZagAnimationDependencies sDefaultDeps(sDefaultStepTimeSource, sDefaultColorSource);
}

// All services implement the "IsActive" service, so declare relevant BT GATT glue logic
using ZigZagAnimationIsActive = AnimationIsActiveBinding<Animation::ZigZag, BtServiceId::ZigZag>;
BT_SVC_IS_ACTIVE_CHRC_DEFINE(ZigZagAnimationIsActive);

BT_GATT_SERVICE_DEFINE(zigzag_anim_service,
                       BT_SVC_UUID_REFERENCE(ZigZagAnimation),
                       BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(ZigZagAnimation, 0, "Step Time Ms"),
                       BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(ZigZagAnimation, 1, "Color"),
                       BT_SVC_IS_ACTIVE_CHRC_REFERENCE(ZigZagAnimationIsActive), );

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