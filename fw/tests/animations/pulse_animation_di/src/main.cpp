#include <animations/animation_renderer.h>
#include <animations/pulse_animation.h>
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

PixelColor sPixelColor;

class CapturingTestRenderer : public AnimationRenderer {
   public:
    size_t displayWidth() const override { return 2; }
    size_t displayHeight() const override { return 1; }
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
        ARG_UNUSED(x);
        ARG_UNUSED(y);
        // Pulse writes the same color to every pixel every tick, so the last
        // write observed is representative of the whole frame.
        sPixelColor.red = r;
        sPixelColor.green = g;
        sPixelColor.blue = b;
    }
};

void reset_capture() {
    sPixelColor = {};
}
}  // namespace

ZTEST_SUITE(pulse_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(pulse_animation_di_tests, test_injected_color_and_period_at_cycle_start_is_dark) {
    MutableUint32Source color(0xFFFFFF);
    MutableUint32Source periodMs(1000);
    PulseAnimationDependencies deps(color, periodMs);

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;
    reset_capture();
    animation->tick(renderer, 0);

    zassert_equal(sPixelColor.red, 0, "Expected panel dark at the start of the breathing cycle");
    zassert_equal(sPixelColor.green, 0, "Expected panel dark at the start of the breathing cycle");
    zassert_equal(sPixelColor.blue, 0, "Expected panel dark at the start of the breathing cycle");
}

ZTEST(pulse_animation_di_tests, test_injected_period_reaches_full_brightness_at_half_cycle) {
    MutableUint32Source color(0x112233);
    MutableUint32Source periodMs(1000);
    PulseAnimationDependencies deps(color, periodMs);

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;
    reset_capture();
    animation->tick(renderer, 500);  // exactly half of period_ms -> peak of the triangle wave

    zassert_equal(sPixelColor.red, 0x11, "Expected full-brightness injected red component");
    zassert_equal(sPixelColor.green, 0x22, "Expected full-brightness injected green component");
    zassert_equal(sPixelColor.blue, 0x33, "Expected full-brightness injected blue component");
}

ZTEST(pulse_animation_di_tests, test_injected_period_changes_phase_at_fixed_elapsed_time) {
    MutableUint32Source color(0xFFFFFF);
    MutableUint32Source periodMs(1000);
    PulseAnimationDependencies deps(color, periodMs);

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;
    reset_capture();
    animation->tick(renderer, 100);  // 100/1000 = 10% into the cycle
    PixelColor slowPeriod = sPixelColor;

    periodMs.set(200);
    animation->init();

    reset_capture();
    animation->tick(renderer, 100);  // 100/200 = 50% into the cycle -> peak
    PixelColor fastPeriod = sPixelColor;

    zassert_true(fastPeriod.red > slowPeriod.red,
                "Expected a shorter injected period to reach a brighter point sooner for the "
                "same elapsed time");
}

ZTEST(pulse_animation_di_tests, test_period_wraps_cleanly_across_multiple_cycles) {
    MutableUint32Source color(0xFFFFFF);
    MutableUint32Source periodMs(1000);
    PulseAnimationDependencies deps(color, periodMs);

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;
    reset_capture();
    animation->tick(renderer, 2500);  // 2.5 cycles -> lands at the same phase as 500ms (peak)

    zassert_equal(sPixelColor.red, 255, "Expected the cycle to wrap and land back at peak");
    zassert_equal(sPixelColor.green, 255, "Expected the cycle to wrap and land back at peak");
    zassert_equal(sPixelColor.blue, 255, "Expected the cycle to wrap and land back at peak");
}

ZTEST(pulse_animation_di_tests, test_zero_period_does_not_crash) {
    MutableUint32Source color(0xFFFFFF);
    MutableUint32Source periodMs(0);
    PulseAnimationDependencies deps(color, periodMs);

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;
    reset_capture();

    // Must not divide-by-zero / crash; the animation clamps period_ms to at
    // least 1ms internally.
    animation->tick(renderer, 5);
}
