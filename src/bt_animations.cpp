#include <bt_animations.h>
#include <led_controller.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bt_anim, LOG_LEVEL_INF);

void BtAdvertisingAnimation::init() {
    currentCycleTimeMs = 0;
}

void BtAdvertisingAnimation::tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) {

    // What should the current brightness be?
    currentCycleTimeMs += timeSinceLastTickMs;

    // Wrap around animation cycle
    if (currentCycleTimeMs > kFadeTimeMs) {
        currentCycleTimeMs = 0;
    }

    // Current brightness: 
    // if less than half of kFadeTimeMs, we are getting brighter
    // if more than half of kFadeTimeMS, we are getting dimmer
    size_t currentBrightness = 0;
    
    if (currentCycleTimeMs < kFadeHalfTimeMs) {
        currentBrightness = kMinFade + (kFadeDistance * ((float)currentCycleTimeMs) / ((float)kFadeHalfTimeMs));
    } else {
        currentBrightness = kMaxFade - (kFadeDistance * ((float)(currentCycleTimeMs-kFadeHalfTimeMs)) / ((float)kFadeHalfTimeMs));
    }

    for (size_t x = 0; x < config->displayWidth; x++) {
        for (size_t y = 0; y < config->displayHeight; y++) {
            set_pixel_in_framebuffer(config, x, y, bufferId, 0, 0, currentBrightness);
        }
    }
}

void BtConnectingAnimation::init() {
    isBrightFlash = false;
    currentCycleTimeMs = 0;
}

void BtConnectingAnimation::tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) {

    // What should the current brightness be?
    currentCycleTimeMs += timeSinceLastTickMs;

    // Wrap around animation cycle
    if (currentCycleTimeMs > kFlashSpeedMs) {
        // Invert the state of what we are flashing
        isBrightFlash = !isBrightFlash;
        // Wrap around
        currentCycleTimeMs = 0;
    }

    size_t currentBrightness = 0;
    
    if (isBrightFlash) {
        currentBrightness = kMaxFlash;
    } else {
        currentBrightness = kMinFlash;
    }

    for (size_t x = 0; x < config->displayWidth; x++) {
        for (size_t y = 0; y < config->displayHeight; y++) {
            set_pixel_in_framebuffer(config, x, y, bufferId, 0, 0, currentBrightness);
        }
    }
}