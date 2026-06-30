#include <animations/tilt_animation.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/sys/__assert.h>

#include <algorithm>

#if defined(CONFIG_LED_STRIP_RGB_SCRATCH)
#define LED_RGB(r, g, b) \
    { 0, r, g, b }
#else
#define LED_RGB(r, g, b) \
    { r, g, b }
#endif

/* ROYGBIV palette — same colors as RainbowAnimation. */
static const struct led_rgb kRainbowColors[] = {
    LED_RGB(255, 0, 0),    /* Red    */
    LED_RGB(255, 165, 0),  /* Orange */
    LED_RGB(255, 255, 0),  /* Yellow */
    LED_RGB(0, 255, 0),    /* Green  */
    LED_RGB(0, 0, 255),    /* Blue   */
    LED_RGB(75, 0, 255),   /* Indigo */
    LED_RGB(143, 0, 200),  /* Violet */
};
static constexpr size_t kNumColors = ARRAY_SIZE(kRainbowColors);

/* Width in pixels over which each color blends into the next. */
static constexpr size_t kRainbowWidthPix = 5;
static constexpr size_t kTotalRainbowWidth = kNumColors * kRainbowWidthPix;

/* 1g in m/s² — used to normalise accel_x to [-1, +1]. */
static constexpr float kGravity = 9.81f;

void TiltAnimation::setImuSource(AnimationImuSource *source) {
    imuSource_ = source;
}

void TiltAnimation::init() {
    tiltOffset_ = 0;
}

void TiltAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    ARG_UNUSED(timeSinceLastTickMs);

    if (!imuSource_) {
        for (size_t x = 0; x < renderer.displayWidth(); x++) {
            for (size_t y = 0; y < renderer.displayHeight(); y++) {
                renderer.setPixel(x, y, 0, 0, 0);
            }
        }
        return;
    }

    imuSource_->update();

    /* Map accel_x (lateral tilt, ±1g) to a rainbow hue offset.
     * accel_x ≈ 0 when glasses are flat → middle of the rainbow cycle.
     * accel_x ≈ ±9.81 when tilted 90° → opposite ends of the cycle. */
    float ax = imuSource_->getAccelX();
    float normalized = (ax / kGravity + 1.0f) * 0.5f;  /* [0.0, 1.0] */
    normalized = std::clamp(normalized, 0.0f, 1.0f);
    tiltOffset_ = (size_t)(normalized * (float)(kTotalRainbowWidth - 1)) % kTotalRainbowWidth;

    for (size_t x = 0; x < renderer.displayWidth(); x++) {
        size_t pos = (x + tiltOffset_) % kTotalRainbowWidth;
        size_t colorIdx = pos / kRainbowWidthPix;
        size_t nextColorIdx = (colorIdx + 1) % kNumColors;
        float blend = (float)(pos % kRainbowWidthPix) / (float)kRainbowWidthPix;

        float r = (1.0f - blend) * (float)kRainbowColors[colorIdx].r +
                  blend * (float)kRainbowColors[nextColorIdx].r;
        float g = (1.0f - blend) * (float)kRainbowColors[colorIdx].g +
                  blend * (float)kRainbowColors[nextColorIdx].g;
        float b = (1.0f - blend) * (float)kRainbowColors[colorIdx].b +
                  blend * (float)kRainbowColors[nextColorIdx].b;

        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            renderer.setPixel(x, y, (uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
}
