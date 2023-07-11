#include <animations/zigzag_animation.h>
#include <bluetooth/read_write_variable.h>

ANIM_SVC_UUID_DEFINE(ZigZagAnimation);

using StepTimeMs = ANIM_SVC_READ_WRITE_VAR_CHRC_DEFINE(ZigZagAnimation, 0, uint32_t, 100);

// All services implement the "IsActive" service, so declare relevant BT GATT glue logic
ANIM_SVC_IS_ACTIVE_CHRC_DEFINE(ZigZagAnimation);

BT_GATT_SERVICE_DEFINE(zigzag_anim_service,
    ANIM_SVC_UUID_REFERENCE(ZigZagAnimation),
    ANIM_SVC_READ_WRITE_VAR_CHRC_REFERENCE(ZigZagAnimation, 0, "Step Time Ms"),
    ANIM_SVC_IS_ACTIVE_CHRC_REFERENCE(ZigZagAnimation),
);


void ZigZagAnimation::init() {
    currentCycleTimeMs = 0;
    currentIndex = 0;
}

void ZigZagAnimation::tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) {
    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > (uint32_t)StepTimeMs::getInstance()) {
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