#include <pattern_controller.h>

#include <led_controller.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <animations/bt_animations.h>
#include <animations/null_animation.h>
#include <animations/zigzag_animation.h>
#include <animations/text_animation.h>
#include <animations/rainbow_animation.h>

LOG_MODULE_REGISTER(pattern_controller, LOG_LEVEL_INF);

void pattern_controller_thread_func(void* a, void* b, void* c);

K_THREAD_DEFINE(
    pattern_controller_thread, 
    4096,
    pattern_controller_thread_func,
    NULL,
    NULL,
    NULL,
    6,
    0,
    0
);

Indicator currentIndicator = Indicator::None;
Animation currentAnimation = Animation::None;

BaseAnimation* getIndicator(Indicator indicator) {
    switch (indicator) {
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

BaseAnimation* getAnimation(Animation animation) {
    switch (animation) {
        case Animation::ZigZag:
            return ZigZagAnimation::getInstance();

        case Animation::Text:
            return TextAnimation::getInstance();

        case Animation::Rainbow:
            return RainbowAnimation::getInstance();
    }

    return NULL;
}

BaseAnimation* getBestRenderAnimation() {
    // Indicators have priority over other animation types
    if (currentIndicator != Indicator::None) {
        return getIndicator(currentIndicator);
    }

    return getAnimation(currentAnimation);
}

void pattern_controller_thread_func(void* a, void* b, void* c) {

    constexpr float kTargetRenderIntervalMs = 66.6f;
    int ret;

    LOG_INF("Pattern control thread start!");

    // Initialize all animations
    BtAdvertisingAnimation::getInstance()->init();
    BtConnectingAnimation::getInstance()->init();
    BtPairingAnimation::getInstance()->init();

    ZigZagAnimation::getInstance()->init();
    NullAnimation::getInstance()->init();
    TextAnimation::getInstance()->init();
    RainbowAnimation::getInstance()->init();

    // Start in the ZigZag animation
    pattern_controller_change_to_animation(Animation::ZigZag);

    while (true) {
        int64_t startTicks = k_uptime_ticks();

        size_t bufferId = 0;
        ret = claimBufferForRender(bufferId);
        if (ret) {
            LOG_ERR("Failed to acquire render buffer!");
        } else {

            BaseAnimation* anim = getBestRenderAnimation();

            if (anim) {
                anim->tick(get_current_led_config(), kTargetRenderIntervalMs, bufferId);
            } else {
                // No animation: default to all LEDs off to save power
                NullAnimation::getInstance()->tick(get_current_led_config(), kTargetRenderIntervalMs, bufferId);
            }

            ret = releaseBufferFromRender(bufferId);
            if (ret) {
                LOG_ERR("Failed to release render buffer!");
            }
        }

        int64_t endTicks = k_uptime_ticks();
        int64_t updateTicks = endTicks - startTicks;
        
        float updateTimeS = ((float)updateTicks) / ((float)CONFIG_SYS_CLOCK_TICKS_PER_SEC);
        float updateTimeMs = updateTimeS * 1000.0f;

        if (updateTimeMs > kTargetRenderIntervalMs) {
            LOG_WRN("Render update took >kTargetRenderIntervalMs, cannot keep render rate!");
        } else {
            // Sleep for however much time is left
            k_msleep(kTargetRenderIntervalMs - updateTimeMs);
        }
    }
}

int pattern_controller_request_indicator(Indicator ind) {
    currentIndicator = ind;
    return 0;
}

int pattern_controller_reset_indicator() {
    currentIndicator = Indicator::None;
    return 0;
}

Animation pattern_controller_get_current_animation(void) {
    return currentAnimation;
}

int pattern_controller_change_to_animation(Animation animation) {
    // Try to get the next animation
    BaseAnimation* next = getAnimation(animation);

    if (!next) {
        LOG_ERR("Cannot change to animation %d", (size_t)animation);
        return -ENOEXEC; // Bail early: we failed to get a pointer to our next animation
    }

    // Mark the current animation as inactive, if possible
    BaseAnimation* curr = getAnimation(currentAnimation);
    if (curr) {
        curr->setActive(false);
    }

    next->init();
    next->setActive(true);

    currentAnimation = animation;

    return 0;
}