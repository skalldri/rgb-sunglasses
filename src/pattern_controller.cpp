#include <pattern_controller.h>

#include <led_controller.h>

#include <core_config.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <animations/animation_registry.h>
#include <animations/bt_animations.h>
#include <animations/null_animation.h>

LOG_MODULE_REGISTER(pattern_controller, LOG_LEVEL_INF);

void pattern_controller_thread_func(void *a, void *b, void *c);

K_THREAD_DEFINE(
    pattern_controller_thread,
    4096,
    pattern_controller_thread_func,
    NULL,
    NULL,
    NULL,
    6,
    0,
    0);

Indicator currentIndicator = Indicator::None;
Animation currentAnimation = Animation::None;

float sBrightnessForFrame = 0.1f;

BaseAnimation *getIndicator(Indicator indicator)
{
    switch (indicator)
    {
    case Indicator::BtConnecting:
        return BtConnectingAnimation::getInstance();

    case Indicator::BtAdvertising:
        return BtAdvertisingAnimation::getInstance();

    case Indicator::BtPairing:
        return BtPairingAnimation::getInstance();

    case Indicator::None:
        // Explicit fallthrough to get to the NULL animation
        break;
    }

    return NULL;
}

BaseAnimation *getAnimation(Animation animation)
{
    return animation_registry_get(animation);
}

BaseAnimation *getBestRenderAnimation()
{
    // Indicators have priority over other animation types
    if (currentIndicator != Indicator::None)
    {
        return getIndicator(currentIndicator);
    }

    return getAnimation(currentAnimation);
}

void pattern_controller_thread_func(void *a, void *b, void *c)
{

    int ret;

    // LOG_INF("Pattern control thread start!");

    // Initialize all animations
    BtAdvertisingAnimation::getInstance()->init();
    BtConnectingAnimation::getInstance()->init();
    BtPairingAnimation::getInstance()->init();

    ret = animation_registry_register_defaults();
    if (ret)
    {
        LOG_ERR("Failed to register default animations: %d", ret);
    }
    else
    {
        animation_registry_init_registered();
    }

    // Start in the ZigZag animation
    pattern_controller_change_to_animation(Animation::ZigZag);

    while (true)
    {
        int64_t startTicks = k_uptime_ticks();

        float kTargetRenderIntervalMs = CoreConfig::getInstance().getRenderRateMs();

        size_t bufferId = 0;
        ret = claimBufferForRender(bufferId);
        if (ret)
        {
            LOG_ERR("Failed to acquire render buffer!");
        }
        else
        {

            BaseAnimation *anim = getBestRenderAnimation();

            // Latch current brightness value from the core config
            sBrightnessForFrame = CoreConfig::getInstance().getBrightnessFactor();

            if (anim)
            {
                anim->tick(get_current_led_config(), kTargetRenderIntervalMs, bufferId);
            }
            else
            {
                // No animation: default to all LEDs off to save power
                BaseAnimation *nullAnimation = animation_registry_get(Animation::None);
                if (nullAnimation)
                {
                    nullAnimation->tick(get_current_led_config(), kTargetRenderIntervalMs, bufferId);
                }
            }

            ret = releaseBufferFromRender(bufferId);
            if (ret)
            {
                LOG_ERR("Failed to release render buffer!");
            }
        }

        int64_t endTicks = k_uptime_ticks();
        int64_t updateTicks = endTicks - startTicks;

        float updateTimeS = ((float)updateTicks) / ((float)CONFIG_SYS_CLOCK_TICKS_PER_SEC);
        float updateTimeMs = updateTimeS * 1000.0f;

        if (updateTimeMs > kTargetRenderIntervalMs)
        {
            LOG_WRN("Render update took >kTargetRenderIntervalMs, cannot keep render rate!");
        }
        else
        {
            // Sleep for however much time is left
            k_msleep(kTargetRenderIntervalMs - updateTimeMs);
        }
    }
}

int pattern_controller_request_indicator(Indicator ind)
{
    currentIndicator = ind;
    return 0;
}

int pattern_controller_reset_indicator()
{
    currentIndicator = Indicator::None;
    return 0;
}

Animation pattern_controller_get_current_animation(void)
{
    return currentAnimation;
}

int pattern_controller_change_to_animation(Animation animation)
{
    // Try to get the next animation
    BaseAnimation *next = getAnimation(animation);

    if (!next)
    {
        LOG_ERR("Cannot change to animation %d", (size_t)animation);
        return -ENOEXEC; // Bail early: we failed to get a pointer to our next animation
    }

    // Mark the current animation as inactive, if possible
    BaseAnimation *curr = getAnimation(currentAnimation);
    if (curr)
    {
        curr->setActive(false);
    }

    next->init();
    next->setActive(true);

    currentAnimation = animation;

    return 0;
}

int pattern_controller_set_pixel_in_framebuffer(const LedConfig *config, size_t x, size_t y, size_t bufferId, uint8_t red, uint8_t green, uint8_t blue)
{
    // Scale colors by the current brightness before submitting to the LED controller
    return set_pixel_in_framebuffer(config, x, y, bufferId, ((float)red) * sBrightnessForFrame, ((float)green) * sBrightnessForFrame, ((float)blue) * sBrightnessForFrame);
}