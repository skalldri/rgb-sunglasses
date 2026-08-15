#include <animations/animation_renderer.h>
#include <animations/rainbow_animation.h>
#include <zephyr/ztest.h>

namespace {
class MutableUint32Source : public AnimationUint32ParameterSource {
   public:
    explicit MutableUint32Source(uint32_t value) : value_(value) {}

    uint32_t get() const override { return value_; }

    void set(uint32_t value) { value_ = value; }

   private:
    uint32_t value_;
};

struct PixelColor {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
};

PixelColor sPixelColors[8];

class CapturingTestRenderer : public AnimationRenderer {
   public:
    size_t displayWidth() const override { return 4; }
    size_t displayHeight() const override { return 1; }
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
        ARG_UNUSED(y);
        if (x < ARRAY_SIZE(sPixelColors)) {
            sPixelColors[x].red = r;
            sPixelColors[x].green = g;
            sPixelColors[x].blue = b;
        }
    }
};

void reset_capture() {
    for (size_t i = 0; i < ARRAY_SIZE(sPixelColors); i++) {
        sPixelColors[i] = {};
    }
}
}  // namespace

ZTEST_SUITE(rainbow_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(rainbow_animation_di_tests, test_injected_step_time_controls_animation_advance) {
    MutableUint32Source stepTimeMs(1000);
    MutableUint32Source rainbowWidthPix(1);
    RainbowAnimationDependencies deps(stepTimeMs, rainbowWidthPix);

    RainbowAnimation *animation = RainbowAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;

    reset_capture();
    animation->tick(renderer, 1);
    PixelColor firstTickX0 = sPixelColors[0];

    reset_capture();
    animation->tick(renderer, 1);
    PixelColor secondTickX0 = sPixelColors[0];

    zassert_equal(firstTickX0.red, secondTickX0.red,
                  "Expected same x0 red channel without advancing");
    zassert_equal(firstTickX0.green, secondTickX0.green,
                  "Expected same x0 green channel without advancing");
    zassert_equal(firstTickX0.blue, secondTickX0.blue,
                  "Expected same x0 blue channel without advancing");

    stepTimeMs.set(0);  // "fastest" = kFastestStepTimeMs (11 ms/step)

    reset_capture();
    animation->tick(renderer, 12);  // renders the old step, then advances (12 > 11)
    PixelColor thirdTickX0 = sPixelColors[0];

    reset_capture();
    animation->tick(renderer, 12);
    PixelColor fourthTickX0 = sPixelColors[0];

    zassert_true(
        (thirdTickX0.red == secondTickX0.red) && (thirdTickX0.green == secondTickX0.green) &&
            (thirdTickX0.blue == secondTickX0.blue),
        "Expected the first tick after step-time change to render the previous rainbow step");

    zassert_false((fourthTickX0.red == secondTickX0.red) &&
                      (fourthTickX0.green == secondTickX0.green) &&
                      (fourthTickX0.blue == secondTickX0.blue),
                  "Expected x0 color to change once injected step time allows advancing");
}

ZTEST(rainbow_animation_di_tests, test_injected_width_controls_gradient) {
    MutableUint32Source stepTimeMs(1000);
    MutableUint32Source rainbowWidthPix(1);
    RainbowAnimationDependencies deps(stepTimeMs, rainbowWidthPix);

    RainbowAnimation *animation = RainbowAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;

    reset_capture();
    animation->tick(renderer, 1);
    PixelColor widthOneX1 = sPixelColors[1];

    rainbowWidthPix.set(2);

    reset_capture();
    animation->init();
    animation->tick(renderer, 1);
    PixelColor widthTwoX1 = sPixelColors[1];

    zassert_false((widthOneX1.red == widthTwoX1.red) && (widthOneX1.green == widthTwoX1.green) &&
                      (widthOneX1.blue == widthTwoX1.blue),
                  "Expected x=1 color to change when injected width changes");
}

// PR #378 review round 8: rainbow/width_pixels is remotely writable with no range
// validation and persists, and tick() uses it as a DIVISOR — an accepted 0 made
// every subsequent render divide by zero (SIGFPE right here on native_sim; on the
// M33, UDIV yields 0 and the inf/NaN blend narrows to uint8_t as UB). The floor
// must make 0 render exactly as 1, not crash.
ZTEST(rainbow_animation_di_tests, test_zero_width_floored_not_divide_by_zero) {
    MutableUint32Source stepTimeMs(1000);
    MutableUint32Source rainbowWidthPix(1);
    RainbowAnimationDependencies deps(stepTimeMs, rainbowWidthPix);

    RainbowAnimation *animation = RainbowAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;

    reset_capture();
    animation->tick(renderer, 1);
    PixelColor widthOneX0 = sPixelColors[0];
    PixelColor widthOneX1 = sPixelColors[1];

    rainbowWidthPix.set(0);

    reset_capture();
    animation->init();
    animation->tick(renderer, 1);  // unfloored, this tick dies with SIGFPE

    zassert_true(sPixelColors[0].red == widthOneX0.red &&
                     sPixelColors[0].green == widthOneX0.green &&
                     sPixelColors[0].blue == widthOneX0.blue,
                 "Width 0 must render exactly as the floored width 1 at x=0");
    zassert_true(sPixelColors[1].red == widthOneX1.red &&
                     sPixelColors[1].green == widthOneX1.green &&
                     sPixelColors[1].blue == widthOneX1.blue,
                 "Width 0 must render exactly as the floored width 1 at x=1");
}

namespace {
bool same_color(const PixelColor &a, const PixelColor &b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}
}  // namespace

// Issue #376: a step time shorter than the tick interval must take several steps in
// one tick (carry-remainder accumulator), not be floored to one step per tick.
ZTEST(rainbow_animation_di_tests, test_step_time_below_tick_advances_multiple_steps) {
    MutableUint32Source stepTimeMs(10);
    MutableUint32Source rainbowWidthPix(1);  // one rainbow step shifts colors one pixel
    RainbowAnimationDependencies deps(stepTimeMs, rainbowWidthPix);

    RainbowAnimation *animation = RainbowAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;

    // The tick renders BEFORE advancing, so this frame shows step 0...
    reset_capture();
    animation->tick(renderer, 33);
    PixelColor x1BeforeAdvance = sPixelColors[1];
    PixelColor x2BeforeAdvance = sPixelColors[2];

    // ...and one 33 ms tick at the 11 ms floor (the 10 ms setting is floored to
    // kFastestStepTimeMs) advanced exactly 2 steps, so the next frame's x0 must
    // show what x2 (not x1) showed before.
    reset_capture();
    animation->tick(renderer, 0);  // render-only: 33-22=11 ms remainder, not > step
    PixelColor x0AfterAdvance = sPixelColors[0];

    zassert_true(same_color(x0AfterAdvance, x2BeforeAdvance),
                 "Expected 2 steps from one 33 ms tick at the 11 ms floor");
    zassert_false(same_color(x0AfterAdvance, x1BeforeAdvance),
                  "Expected more than 1 step from one 33 ms tick at the 11 ms floor");
}

// Issue #376: total displacement must depend only on total elapsed time, not on how
// that time is partitioned into ticks (90 Hz and 30 Hz must render the same motion).
ZTEST(rainbow_animation_di_tests, test_equal_displacement_across_tick_rates) {
    MutableUint32Source stepTimeMs(45);  // divides neither 11 nor 33 evenly
    MutableUint32Source rainbowWidthPix(1);
    RainbowAnimationDependencies deps(stepTimeMs, rainbowWidthPix);

    RainbowAnimation *animation = RainbowAnimation::getInstance();
    animation->setDependencies(deps);
    CapturingTestRenderer renderer;

    // 990 ms as 90 ticks of 11 ms (the old ~90 Hz render rate)...
    animation->init();
    for (int i = 0; i < 90; i++) {
        animation->tick(renderer, 11);
    }
    reset_capture();
    animation->tick(renderer, 0);  // render-only tick to observe the final position
    PixelColor x0At90Hz = sPixelColors[0];

    // ...and as 30 ticks of 33 ms (the ~30 Hz render rate).
    animation->init();
    for (int i = 0; i < 30; i++) {
        animation->tick(renderer, 33);
    }
    reset_capture();
    animation->tick(renderer, 0);
    PixelColor x0At30Hz = sPixelColors[0];

    zassert_true(same_color(x0At90Hz, x0At30Hz),
                 "Displacement must not depend on tick partitioning");
    // floor((990-1)/45) = 21 steps either way; (21 + 0) % 7 colors = red again.
    zassert_equal(x0At90Hz.red, 255, "Expected exactly 21 steps in 990 ms (red)");
    zassert_equal(x0At90Hz.green, 0, "Expected exactly 21 steps in 990 ms (red)");
    zassert_equal(x0At90Hz.blue, 0, "Expected exactly 21 steps in 990 ms (red)");
}
