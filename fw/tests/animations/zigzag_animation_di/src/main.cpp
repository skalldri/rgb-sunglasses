#include <animations/animation_renderer.h>
#include <animations/zigzag_animation.h>
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

struct PixelCapture {
    size_t x = 0;
    size_t y = 0;
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    size_t litPixelWrites = 0;
};

PixelCapture sCapture;

class CapturingTestRenderer : public AnimationRenderer {
   public:
    size_t displayWidth() const override { return 2; }
    size_t displayHeight() const override { return 1; }
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
        if (r != 0 || g != 0 || b != 0) {
            sCapture.x = x;
            sCapture.y = y;
            sCapture.red = r;
            sCapture.green = g;
            sCapture.blue = b;
            sCapture.litPixelWrites++;
        }
    }
};

void reset_capture() {
    sCapture = {};
}

// 8x1 display so multi-step advances are distinguishable without wrapping.
class WideTestRenderer : public CapturingTestRenderer {
   public:
    size_t displayWidth() const override { return 8; }
};
}  // namespace

ZTEST_SUITE(zigzag_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(zigzag_animation_di_tests, test_injected_step_time_advances_pixel) {
    MutableUint32Source stepTimeMs(1);
    MutableUint32Source color(0x112233);
    ZigZagAnimationDependencies deps(stepTimeMs, color);

    ZigZagAnimation *animation = ZigZagAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;
    reset_capture();
    animation->tick(renderer, 2);

    zassert_equal(sCapture.litPixelWrites, 1, "Expected exactly one lit pixel write");
    zassert_equal(sCapture.x, 1, "Expected lit pixel to advance to x=1");
    zassert_equal(sCapture.y, 0, "Expected lit pixel to remain on row 0");
    zassert_equal(sCapture.red, 0x11, "Expected injected red component");
    zassert_equal(sCapture.green, 0x22, "Expected injected green component");
    zassert_equal(sCapture.blue, 0x33, "Expected injected blue component");
}

ZTEST(zigzag_animation_di_tests, test_injected_step_time_holds_pixel_when_not_elapsed) {
    MutableUint32Source stepTimeMs(1000);
    MutableUint32Source color(0xAA5500);
    ZigZagAnimationDependencies deps(stepTimeMs, color);

    ZigZagAnimation *animation = ZigZagAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;
    reset_capture();
    animation->tick(renderer, 1);

    zassert_equal(sCapture.litPixelWrites, 1, "Expected exactly one lit pixel write");
    zassert_equal(sCapture.x, 0, "Expected pixel to remain at x=0 when step time has not elapsed");
    zassert_equal(sCapture.red, 0xAA, "Expected injected red component");
    zassert_equal(sCapture.green, 0x55, "Expected injected green component");
    zassert_equal(sCapture.blue, 0x00, "Expected injected blue component");
}

ZTEST(zigzag_animation_di_tests, test_pixel_wraps_to_first_index_after_all_indices) {
    // 2x1 display → 2 total indices. An 11 ms tick at a 10 ms step advances one
    // index per tick (11 > 10, remainder 1; 12 > 10, remainder 2).
    // init: index=0; tick 1 → index=1; tick 2 → index=2 wraps to 0.
    MutableUint32Source stepTimeMs(10);
    MutableUint32Source color(0xFF0000);
    ZigZagAnimationDependencies deps(stepTimeMs, color);

    ZigZagAnimation *animation = ZigZagAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;

    reset_capture();
    animation->tick(renderer, 11);  // advances to index 1

    reset_capture();
    animation->tick(renderer, 11);  // advances to index 2 → wraps to 0

    zassert_equal(sCapture.litPixelWrites, 1, "Expected exactly one lit pixel write after wrap");
    zassert_equal(sCapture.x, 0, "Expected lit pixel at x=0 after wrapping");
    zassert_equal(sCapture.y, 0, "Expected lit pixel at y=0 after wrapping");
}

// PR #378 review: a step time of 0 means "fastest" = the HISTORICAL fastest,
// kFastestStepTimeMs (11 ms/step, one step per tick at the old 90 Hz render
// rate) — wall-clock defined at any tick rate, and the same speed users have
// always had. NOT "one step per tick" (3x slower at a 33 ms tick) and NOT
// 1 ms (11-33x faster than any previous firmware).
ZTEST(zigzag_animation_di_tests, test_zero_step_time_is_wall_clock_fastest) {
    MutableUint32Source stepTimeMs(0);
    MutableUint32Source color(0xFF0000);
    ZigZagAnimationDependencies deps(stepTimeMs, color);

    ZigZagAnimation *animation = ZigZagAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    WideTestRenderer renderer;
    reset_capture();
    // 33 ms at 11 ms/step: 33 → 22 → 11 (11 > 11 is false), exactly 2 steps.
    animation->tick(renderer, 33);
    zassert_equal(sCapture.x, 2, "Expected 2 11-ms steps from one 33 ms tick");

    reset_capture();
    animation->tick(renderer, 12);  // 11 carried + 12 = 23 → 2 more steps → x=4
    zassert_equal(sCapture.x, 4, "Expected 2 further steps from a 12 ms tick");
}

// Issue #376: a step time shorter than the tick interval must take several steps in
// one tick (carry-remainder accumulator), not be floored to one step per tick.
ZTEST(zigzag_animation_di_tests, test_step_time_below_tick_advances_multiple_steps) {
    MutableUint32Source stepTimeMs(10);
    MutableUint32Source color(0xFF0000);
    ZigZagAnimationDependencies deps(stepTimeMs, color);

    ZigZagAnimation *animation = ZigZagAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    WideTestRenderer renderer;
    reset_capture();
    // One 33 ms tick with a 10 ms step: 33 → 23 → 13 → 3, i.e. exactly 3 steps.
    animation->tick(renderer, 33);

    zassert_equal(sCapture.x, 3, "Expected 3 steps from one 33 ms tick at a 10 ms step time");
}

// PR #378 review: the accumulator is clamped to one step plus one tick, so a
// huge-then-small remotely written step time cannot run accumulated/step loop
// iterations (nor take a burst of steps) in a single tick. Same clamp in
// rainbow and text; zigzag is the representative test.
ZTEST(zigzag_animation_di_tests, test_step_time_change_does_not_burst) {
    MutableUint32Source stepTimeMs(1000000);  // effectively "never step"
    MutableUint32Source color(0xFF0000);
    ZigZagAnimationDependencies deps(stepTimeMs, color);

    ZigZagAnimation *animation = ZigZagAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    WideTestRenderer renderer;
    for (int i = 0; i < 5; i++) {
        animation->tick(renderer, 200);  // accumulate 1000 ms with no steps
    }

    // Drop to a 10 ms step: the clamp caps the accumulator at step + dt = 43,
    // so this tick takes exactly floor((43-1)/10) = 4 steps — not the ~100 the
    // stale 1000 ms accumulator would otherwise pay for in one tick.
    stepTimeMs.set(10);
    reset_capture();
    animation->tick(renderer, 33);
    zassert_equal(sCapture.x, 4, "Expected 4 steps after the step-time change, not a burst");
}

// Issue #376: total displacement must depend only on total elapsed time, not on how
// that time is partitioned into ticks (90 Hz and 30 Hz must render the same motion).
ZTEST(zigzag_animation_di_tests, test_equal_displacement_across_tick_rates) {
    MutableUint32Source stepTimeMs(45);  // divides neither 11 nor 33 evenly
    MutableUint32Source color(0xFF0000);
    ZigZagAnimationDependencies deps(stepTimeMs, color);

    ZigZagAnimation *animation = ZigZagAnimation::getInstance();
    animation->setDependencies(deps);
    WideTestRenderer renderer;

    // 990 ms as 90 ticks of 11 ms (the old ~90 Hz render rate)...
    animation->init();
    for (int i = 0; i < 90; i++) {
        animation->tick(renderer, 11);
    }
    reset_capture();
    animation->tick(renderer, 0);  // render-only tick to observe the final position
    const size_t xAt90Hz = sCapture.x;

    // ...and as 30 ticks of 33 ms (the ~30 Hz render rate).
    animation->init();
    for (int i = 0; i < 30; i++) {
        animation->tick(renderer, 33);
    }
    reset_capture();
    animation->tick(renderer, 0);
    const size_t xAt30Hz = sCapture.x;

    // floor((990-1)/45) = 21 steps either way; 21 % 8 = index 5.
    zassert_equal(xAt90Hz, xAt30Hz, "Displacement must not depend on tick partitioning");
    zassert_equal(xAt90Hz, 5, "Expected exactly 21 steps in 990 ms at a 45 ms step time");
}
