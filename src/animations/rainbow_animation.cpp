#include <animations/rainbow_animation.h>
#include <animations/animation_is_active_binding.h>

#include <bluetooth/read_write_variable.h>

#include <zephyr/drivers/led_strip.h>

#include <cstddef>

BT_SVC_UUID_DEFINE(RainbowAnimation);

using StepTimeMs = BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(RainbowAnimation, 0, uint32_t, 100);

using RainbowWidthPix = BT_SVC_READ_WRITE_VAR_CHRC_DEFINE(RainbowAnimation, 1, uint32_t, 5);

// All services implement the "IsActive" service, so declare relevant BT GATT glue logic
using RainbowAnimationIsActive = AnimationIsActiveBinding<Animation::Rainbow, BtServiceId::Rainbow>;
BT_SVC_IS_ACTIVE_CHRC_DEFINE(RainbowAnimationIsActive);

BT_GATT_SERVICE_DEFINE(rainbow_anim_service,
                       BT_SVC_UUID_REFERENCE(RainbowAnimation),
                       BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(RainbowAnimation, 0, "Step Time Ms"),
                       BT_SVC_READ_WRITE_VAR_CHRC_REFERENCE(RainbowAnimation, 1, "Rainbow Width Pixels"),
                       BT_SVC_IS_ACTIVE_CHRC_REFERENCE(RainbowAnimationIsActive), );

#if defined(CONFIG_LED_STRIP_RGB_SCRATCH)
#define LED_RGB(r, g, b) {0, r, g, b}
#else
#define LED_RGB(r, g, b) {r, g, b}
#endif

// Rainbow colors: ROYGBIV
// NOTE: they have a scratch value!
const struct led_rgb rainbowColors[] = {
    LED_RGB(255, 0, 0),   // Red
    LED_RGB(255, 165, 0), // Orange
    LED_RGB(255, 255, 0), // Yellow
    LED_RGB(0, 255, 0),   // Green
    LED_RGB(0, 0, 255),   // Blue
    LED_RGB(75, 0, 255),  // Indigo-ish
    LED_RGB(143, 0, 200)  // Violet-ish
};

static constexpr size_t numRainbowColors = ARRAY_SIZE(rainbowColors);

float rainbowBrightness = 0.05f;

void RainbowAnimation::init()
{
    currentCycleTimeMs = 0;
    currentRainbowStep = 0;
}

void RainbowAnimation::tick(const LedConfig *config, const size_t timeSinceLastTickMs, const size_t bufferId)
{
    // Read BT variables
    const uint32_t rainbowColorWidth = (uint32_t)RainbowWidthPix::getInstance();

    // Turn off all LEDs
    for (size_t x = 0; x < config->displayWidth; x++)
    {
        size_t currentRainbowColor = ((currentRainbowStep + x) / rainbowColorWidth) % numRainbowColors;
        size_t nextRainbowColor = (currentRainbowColor + 1) % numRainbowColors;

        // Figure out the blend percentage
        // First: how far are we through the current color, in rainbow steps
        float blendPercent = ((currentRainbowStep + x) % rainbowColorWidth);

        // How far is that as a percentage?
        blendPercent /= (float)rainbowColorWidth;

        float red = ((1.0f - blendPercent) * ((float)rainbowColors[currentRainbowColor].r)) + (blendPercent * ((float)rainbowColors[nextRainbowColor].r));
        float green = ((1.0f - blendPercent) * ((float)rainbowColors[currentRainbowColor].g)) + (blendPercent * ((float)rainbowColors[nextRainbowColor].g));
        float blue = ((1.0f - blendPercent) * ((float)rainbowColors[currentRainbowColor].b)) + (blendPercent * ((float)rainbowColors[nextRainbowColor].b));

        for (size_t y = 0; y < config->displayHeight; y++)
        {
            pattern_controller_set_pixel_in_framebuffer(config, x, y, bufferId, red, green, blue);
        }
    }

    // Add the time to our counter
    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > (uint32_t)StepTimeMs::getInstance())
    {
        currentCycleTimeMs = 0;
        currentRainbowStep++; // Move text one pixel to the left
    }
}