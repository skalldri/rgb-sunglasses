#include <animations/rainbow_animation.h>

#include <zephyr/drivers/led_strip.h>
#include <zephyr/sys/__assert.h>

#include <cstddef>

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

void RainbowAnimation::setDependencies(const RainbowAnimationDependencies &deps)
{
    deps_ = &deps;
}

void RainbowAnimation::init()
{
    currentCycleTimeMs = 0;
    currentRainbowStep = 0;
}

void RainbowAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs)
{
    __ASSERT(deps_, "RainbowAnimation::tick before setDependencies");

    // Read BT variables
    const uint32_t rainbowColorWidth = deps_->rainbowWidthPix.get();

    for (size_t x = 0; x < renderer.displayWidth(); x++)
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

        for (size_t y = 0; y < renderer.displayHeight(); y++)
        {
            renderer.setPixel(x, y, red, green, blue);
        }
    }

    // Add the time to our counter
    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > deps_->stepTimeMs.get())
    {
        currentCycleTimeMs = 0;
        currentRainbowStep++; // Move text one pixel to the left
    }
}
