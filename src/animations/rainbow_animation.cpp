#include <animations/rainbow_animation.h>

#include <zephyr/drivers/led_strip.h>

#include <cstddef>

ANIM_SVC_UUID_DEFINE(RainbowAnimation);

// All services implement the "IsActive" service, so declare relevant BT GATT glue logic
ANIM_SVC_IS_ACTIVE_CHRC_DEFINE(RainbowAnimation);

BT_GATT_SERVICE_DEFINE(rainbow_anim_service,
    ANIM_SVC_UUID_REFERENCE(RainbowAnimation),
    ANIM_SVC_IS_ACTIVE_CHRC_REFERENCE(RainbowAnimation),
);

// Rainbow colors: ROYGBIV
// NOTE: they have a scratch value!
const struct led_rgb rainbowColors[] = {
    {0, 255, 0, 0},    // Red
    {0, 255, 165, 0},  // Orange
    {0, 255, 255, 0},  // Yellow
    {0, 0, 255, 0},    // Green
    {0, 0, 0, 255},    // Blue
    {0, 75, 0, 255},   // Indigo-ish
    {0, 143, 0, 200}   // Violet-ish
};

static constexpr size_t numRainbowColors = ARRAY_SIZE(rainbowColors);

float rainbowBrightness = 0.05f;

void RainbowAnimation::init() {
    currentCycleTimeMs = 0;
    currentRainbowStep = 0;
}

void RainbowAnimation::tick(const LedConfig* config, const size_t timeSinceLastTickMs, const size_t bufferId) {
    // Turn off all LEDs
    for (size_t x = 0; x < config->displayWidth; x++) {
        size_t currentRainbowColor = ((currentRainbowStep + x) / rainbowColorWidth) % numRainbowColors;
        size_t nextRainbowColor = (currentRainbowColor + 1) % numRainbowColors;

            // Figure out the blend percentage
        // First: how far are we through the current color, in rainbow steps
        float blendPercent = (currentRainbowStep % rainbowColorWidth);

        // How far is that as a percentage?
        blendPercent /= (float)rainbowColorWidth;

        float red = ((1.0f - blendPercent) * ((float)rainbowColors[currentRainbowColor].r)) + (blendPercent * ((float)rainbowColors[nextRainbowColor].r));
        float green = ((1.0f - blendPercent) * ((float)rainbowColors[currentRainbowColor].g)) + (blendPercent * ((float)rainbowColors[nextRainbowColor].g));
        float blue = ((1.0f - blendPercent) * ((float)rainbowColors[currentRainbowColor].b)) + (blendPercent * ((float)rainbowColors[nextRainbowColor].b));

        for (size_t y = 0; y < config->displayHeight; y++) {
            set_pixel_in_framebuffer(config, x, y, bufferId, red, green, blue);
        }
    }

    // Add the time to our counter
    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > stepTime) {
        currentCycleTimeMs = 0;
        currentRainbowStep++; // Move text one pixel to the left
    }
}