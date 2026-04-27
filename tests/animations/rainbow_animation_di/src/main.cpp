#include <zephyr/ztest.h>

#include <animations/rainbow_animation.h>
#include <led_config.h>

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

    constexpr size_t kLedsOnRow[] = {4};
    constexpr size_t kRowStart[] = {0};
    constexpr bool kRowDirection[] = {true};

    const LedConfig kTestConfig = {
        .displayWidth = 4,
        .displayHeight = 1,
        .ledBankWidth = 2,
        .ledsOnRow = kLedsOnRow,
        .rowStartIndex = kRowStart,
        .rowIsLeftToRight = kRowDirection,
    };

    void reset_capture()
    {
        for (size_t i = 0; i < ARRAY_SIZE(sPixelColors); i++)
        {
            sPixelColors[i] = {};
        }
    }
}

int pattern_controller_set_pixel_in_framebuffer(const LedConfig *config, size_t x, size_t y, size_t bufferId, uint8_t red, uint8_t green, uint8_t blue)
{
    ARG_UNUSED(config);
    ARG_UNUSED(y);
    ARG_UNUSED(bufferId);

    if (x < ARRAY_SIZE(sPixelColors))
    {
        sPixelColors[x].red = red;
        sPixelColors[x].green = green;
        sPixelColors[x].blue = blue;
    }

    return 0;
}

int pattern_controller_change_to_animation(Animation animation)
{
    ARG_UNUSED(animation);
    return 0;
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

    reset_capture();
    animation->tick(&kTestConfig, 1, 0);
    PixelColor firstTickX0 = sPixelColors[0];

    reset_capture();
    animation->tick(&kTestConfig, 1, 0);
    PixelColor secondTickX0 = sPixelColors[0];

    zassert_equal(firstTickX0.red, secondTickX0.red, "Expected same x0 red channel without advancing");
    zassert_equal(firstTickX0.green, secondTickX0.green, "Expected same x0 green channel without advancing");
    zassert_equal(firstTickX0.blue, secondTickX0.blue, "Expected same x0 blue channel without advancing");

    stepTimeMs.set(0);

    reset_capture();
    animation->tick(&kTestConfig, 1, 0);
    PixelColor thirdTickX0 = sPixelColors[0];

    reset_capture();
    animation->tick(&kTestConfig, 1, 0);
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

    reset_capture();
    animation->tick(&kTestConfig, 1, 0);
    PixelColor widthOneX1 = sPixelColors[1];

    rainbowWidthPix.set(2);

    reset_capture();
    animation->init();
    animation->tick(&kTestConfig, 1, 0);
    PixelColor widthTwoX1 = sPixelColors[1];

    zassert_false((widthOneX1.red == widthTwoX1.red) && (widthOneX1.green == widthTwoX1.green) && (widthOneX1.blue == widthTwoX1.blue),
                  "Expected x=1 color to change when injected width changes");
}
