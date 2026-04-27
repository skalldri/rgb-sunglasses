#include <zephyr/ztest.h>

#include <animations/rainbow_animation.h>
#include <animations/animation_renderer.h>

namespace
{
    class MutableUint32Source : public AnimationUint32ParameterSource
    {
    public:
        explicit MutableUint32Source(uint32_t value)
            : value_(value)
        {
        }

        uint32_t get() const override
        {
            return value_;
        }

        void set(uint32_t value)
        {
            value_ = value;
        }

    private:
        uint32_t value_;
    };

    struct PixelColor
    {
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
    };

    PixelColor sPixelColors[8];

    class CapturingTestRenderer : public AnimationRenderer
    {
    public:
        size_t displayWidth() const override { return 4; }
        size_t displayHeight() const override { return 1; }
        void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override
        {
            ARG_UNUSED(y);
            if (x < ARRAY_SIZE(sPixelColors))
            {
                sPixelColors[x].red = r;
                sPixelColors[x].green = g;
                sPixelColors[x].blue = b;
            }
        }
    };

    void reset_capture()
    {
        for (size_t i = 0; i < ARRAY_SIZE(sPixelColors); i++)
        {
            sPixelColors[i] = {};
        }
    }
}

ZTEST_SUITE(rainbow_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(rainbow_animation_di_tests, test_injected_step_time_controls_animation_advance)
{
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

    zassert_equal(firstTickX0.red, secondTickX0.red, "Expected same x0 red channel without advancing");
    zassert_equal(firstTickX0.green, secondTickX0.green, "Expected same x0 green channel without advancing");
    zassert_equal(firstTickX0.blue, secondTickX0.blue, "Expected same x0 blue channel without advancing");

    stepTimeMs.set(0);

    reset_capture();
    animation->tick(renderer, 1);
    PixelColor thirdTickX0 = sPixelColors[0];

    reset_capture();
    animation->tick(renderer, 1);
    PixelColor fourthTickX0 = sPixelColors[0];

    zassert_true((thirdTickX0.red == secondTickX0.red) && (thirdTickX0.green == secondTickX0.green) && (thirdTickX0.blue == secondTickX0.blue),
                 "Expected the first tick after step-time change to render the previous rainbow step");

    zassert_false((fourthTickX0.red == secondTickX0.red) && (fourthTickX0.green == secondTickX0.green) && (fourthTickX0.blue == secondTickX0.blue),
                  "Expected x0 color to change once injected step time allows advancing");
}

ZTEST(rainbow_animation_di_tests, test_injected_width_controls_gradient)
{
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

    zassert_false((widthOneX1.red == widthTwoX1.red) && (widthOneX1.green == widthTwoX1.green) && (widthOneX1.blue == widthTwoX1.blue),
                  "Expected x=1 color to change when injected width changes");
}
