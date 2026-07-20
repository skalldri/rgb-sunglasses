#include <animations/tilt_animation.h>
#include <zephyr/drivers/led_strip.h>

#include <math.h>

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
static constexpr float kRainbowWidthPix = 5.0f;
static constexpr float kTotalRainbowWidth = (float)kNumColors * kRainbowWidthPix;

/* Model (gyro-only):
 *
 * The display shows a 2-D "rainbow plaid": two perpendicular rainbow gradients
 * (one along each texture axis) averaged together, so the texture varies across
 * BOTH axes. That matters because a single 1-D gradient has a null direction —
 * motion parallel to its bands is invisible — and roll rotates that null onto one
 * of the scroll axes, hiding it. A 2-D texture has no null direction, so every
 * motion is observable in any orientation.
 *
 * Head motion (from the BMI270 gyroscope, see fw/docs/imu-coordinate-frame.md for
 * the axis frame) moves the texture:
 *   - yaw   (rate about +X, up)   → scroll horizontally      (scrollX_)
 *   - pitch (rate about +Y, left) → scroll vertically        (scrollY_)
 *   - roll  (rate about +Z, back) → rotate the whole texture (angleRad_)
 *
 * Each per-tick angular rate (rad/s) is integrated over the tick duration: scroll
 * rates are scaled to pixels; roll integrates 1:1 into the texture-rotation angle.
 * The scroll offsets are applied in screen space (so yaw always moves the sampling
 * point horizontally and pitch vertically) and the sampling point is then rotated
 * by the roll angle.
 *
 * Gyro-only means no gravity/accel fusion: the pattern responds to rotation rate,
 * not absolute orientation, and slowly drifts with gyro bias. Smoothing / a
 * complementary or Kalman filter is a deliberate follow-up.
 *
 * Per-tick cost is O(width×height): two fmodf + two rainbow lookups per pixel,
 * plus one cosf/sinf per tick. Verified on proto0 to stay within the render
 * budget — no pattern_controller/led_controller over-budget warnings while Tilt
 * is the active animation.
 */

/* Scroll sensitivity: pixels of gradient travel per radian of integrated rotation.
 * ~20 px/rad means a ~90° head turn scrolls roughly one full rainbow. */
static constexpr float kScrollPixelsPerRad = 20.0f;

static constexpr float kTwoPi = 6.2831853f;

/* Sign of each axis' contribution. Verified on-head on proto0 (issue #177): at
 * +1 each of yaw/pitch/roll moves the texture the expected way. Kept as named
 * constants so a future hardware revision that flips an axis is a one-line change. */
static constexpr float kYawSign = 1.0f;
static constexpr float kPitchSign = 1.0f;
static constexpr float kRollSign = 1.0f;

/* Fill (r,g,b) with the interpolated rainbow color at gradient position `pos`
 * (in pixels; wrapped into [0, kTotalRainbowWidth) by the caller). */
static void rainbowColorAt(float pos, uint8_t &r, uint8_t &g, uint8_t &b) {
    size_t colorIdx = (size_t)(pos / kRainbowWidthPix) % kNumColors;
    size_t nextColorIdx = (colorIdx + 1) % kNumColors;
    float blend = (pos - (float)colorIdx * kRainbowWidthPix) / kRainbowWidthPix;

    r = (uint8_t)((1.0f - blend) * (float)kRainbowColors[colorIdx].r +
                  blend * (float)kRainbowColors[nextColorIdx].r);
    g = (uint8_t)((1.0f - blend) * (float)kRainbowColors[colorIdx].g +
                  blend * (float)kRainbowColors[nextColorIdx].g);
    b = (uint8_t)((1.0f - blend) * (float)kRainbowColors[colorIdx].b +
                  blend * (float)kRainbowColors[nextColorIdx].b);
}

/* Wrap a gradient position into [0, kTotalRainbowWidth) — fmodf can return
 * negatives for negative input. */
static float wrapPos(float u) {
    float pos = fmodf(u, kTotalRainbowWidth);
    if (pos < 0.0f) {
        pos += kTotalRainbowWidth;
    }
    return pos;
}

void TiltAnimation::setImuSource(AnimationImuSource *source) {
    imuSource_ = source;
}

void TiltAnimation::init() {
    scrollX_ = 0.0f;
    scrollY_ = 0.0f;
    angleRad_ = 0.0f;
}

void TiltAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    if (!imuSource_) {
        for (size_t x = 0; x < renderer.displayWidth(); x++) {
            for (size_t y = 0; y < renderer.displayHeight(); y++) {
                renderer.setPixel(x, y, 0, 0, 0);
            }
        }
        return;
    }

    imuSource_->update();

    /* Integrate the gyro rates (rad/s) over this tick. */
    float dt = (float)timeSinceLastTickMs / 1000.0f;
    scrollX_ += kYawSign * imuSource_->getGyroX() * dt * kScrollPixelsPerRad;
    scrollY_ += kPitchSign * imuSource_->getGyroY() * dt * kScrollPixelsPerRad;
    angleRad_ += kRollSign * imuSource_->getGyroZ() * dt;
    /* Keep the angle bounded so cosf/sinf stay accurate over long runs. */
    angleRad_ = fmodf(angleRad_, kTwoPi);
    /* scrollX_/scrollY_ are deliberately NOT wrapped the way angleRad_ is: they are
     * screen-space offsets that project onto BOTH rotated axes below, so no modulo
     * of them is seamless at an arbitrary roll angle (it would jump the pattern).
     * Their float32 precision only coarsens after days of continuously-active
     * integration, and init() zeroes them on every (re)activation, so in practice
     * they never reach that range; the planned orientation filter replaces this
     * free-integration outright. */

    float cosA = cosf(angleRad_);
    float sinA = sinf(angleRad_);

    /* Rotate/scroll about the display center so roll spins the texture in place. */
    float cx = (float)(renderer.displayWidth() - 1) * 0.5f;
    float cy = (float)(renderer.displayHeight() - 1) * 0.5f;

    for (size_t x = 0; x < renderer.displayWidth(); x++) {
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            /* Screen-space sampling point, shifted by the (screen-aligned) scroll
             * offsets so yaw moves it horizontally and pitch vertically. */
            float sx = (float)x - cx + scrollX_;
            float sy = (float)y - cy + scrollY_;

            /* Rotate into the texture's frame (roll). p and q are the two
             * perpendicular rainbow axes — sampling both and averaging gives a
             * texture that varies across both axes, so no motion is ever hidden. */
            float p = sx * cosA + sy * sinA;
            float q = -sx * sinA + sy * cosA;

            uint8_t rp, gp, bp, rq, gq, bq;
            rainbowColorAt(wrapPos(p), rp, gp, bp);
            rainbowColorAt(wrapPos(q), rq, gq, bq);

            renderer.setPixel(x, y, (uint8_t)(((uint16_t)rp + rq) / 2),
                              (uint8_t)(((uint16_t)gp + gq) / 2),
                              (uint8_t)(((uint16_t)bp + bq) / 2));
        }
    }
}
