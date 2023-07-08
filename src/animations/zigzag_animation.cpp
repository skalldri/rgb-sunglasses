#include <animations/zigzag_animation.h>

void ZigZagAnimation::init() {
    currentCycleTimeMs = 0;
    currentIndex = 0;
}

void ZigZagAnimation::tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) {
    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > stepTime) {
        currentCycleTimeMs = 0;
        currentIndex++;
    }

    if (currentIndex >= (config->displayWidth * config->displayHeight)) {
        currentIndex = 0;
    }

    // Turn off all LEDs
    for (size_t x = 0; x < config->displayWidth; x++) {
        for (size_t y = 0; y < config->displayHeight; y++) {
            set_pixel_in_framebuffer(config, x, y, bufferId, 0, 0, 0);
        }
    }

    size_t y = currentIndex / config->displayWidth;
    size_t x = currentIndex % config->displayWidth;

    // Turn on just one
    set_pixel_in_framebuffer(config, x, y, bufferId, 50, 50, 50);
}