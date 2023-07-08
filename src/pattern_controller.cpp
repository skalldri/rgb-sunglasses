#include <pattern_controller.h>

#include <led_controller.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <animations/bt_animations.h>
#include <animations/null_animation.h>
#include <animations/zigzag_animation.h>

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

Indicator currentIndicator = Indicator::NONE;
bool indicatorActive = false;

BtAdvertisingAnimation btAdvAnim;
BtConnectingAnimation btConnAnim;
ZigZagAnimation zigZagAnim;
NullAnimation nullAnim;

// TODO: better name
enum class ActiveAnimation {
    None,
    ZigZag,
};

ActiveAnimation currentAnimation = ActiveAnimation::ZigZag;

Animation* getIndicatorAnimation() {
    switch (currentIndicator) {
        case Indicator::BT_CONNECTING:
            return &btConnAnim;

        case Indicator::BT_ADVERTISING:
            return &btAdvAnim;

        case Indicator::NONE:
            // Explicit fallthrough to get to the NULL animation
            break; 
    }

    return NULL;
}

Animation* getCurrentAnimation() {
    if (indicatorActive) {
        return getIndicatorAnimation();
    }

    switch (currentAnimation) {
        case ActiveAnimation::ZigZag:
            return &zigZagAnim;

        case ActiveAnimation::None:
            // Intentional fallthrough to the NULL case
            break;
    }

    return NULL;
}

void pattern_controller_thread_func(void* a, void* b, void* c) {

    constexpr float kTargetRenderIntervalMs = 66.6f;
    int ret;

    LOG_INF("Pattern control thread start!");

    btAdvAnim.init();
    btConnAnim.init();

    while (true) {
        int64_t startTicks = k_uptime_ticks();

        size_t bufferId = 0;
        ret = claimBufferForRender(bufferId);
        if (ret) {
            LOG_ERR("Failed to acquire render buffer!");
        } else {

            Animation* anim = getCurrentAnimation();

            if (anim) {
                anim->tick(get_current_led_config(), kTargetRenderIntervalMs, bufferId);
            } else {
                // No animation: default to all LEDs off to save power
                nullAnim.tick(get_current_led_config(), kTargetRenderIntervalMs, bufferId);
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
    indicatorActive = true;

    return 0;
}

int pattern_controller_reset_indicator() {
    indicatorActive = false;

    return 0;
}