#include <zephyr/ztest.h>

#include <animations/zigzag_animation.h>
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

    struct PixelCapture
    {
        size_t x = 0;
        size_t y = 0;
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        size_t litPixelWrites = 0;
    };

    PixelCapture sCapture;

    constexpr size_t kLedsOnRow[] = {2};
    constexpr size_t kRowStart[] = {0};
    constexpr bool kRowDirection[] = {true};

    const LedConfig kTestConfig = {
        .displayWidth = 2,
        .displayHeight = 1,
        .ledBankWidth = 1,
        .ledsOnRow = kLedsOnRow,
        .rowStartIndex = kRowStart,
        .rowIsLeftToRight = kRowDirection,
    };

    void reset_capture()
    {
        sCapture = {};
    }
}

int pattern_controller_set_pixel_in_framebuffer(const LedConfig *config, size_t x, size_t y, size_t bufferId, uint8_t red, uint8_t green, uint8_t blue)
{
    ARG_UNUSED(config);
    ARG_UNUSED(bufferId);

    if (red != 0 || green != 0 || blue != 0)
    {
        sCapture.x = x;
        sCapture.y = y;
        sCapture.red = red;
        sCapture.green = green;
        sCapture.blue = blue;
        sCapture.litPixelWrites++;
    }

    return 0;
}

int pattern_controller_change_to_animation(Animation animation)
{
    ARG_UNUSED(animation);
    return 0;
}

ZTEST_SUITE(zigzag_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(zigzag_animation_di_tests, test_injected_step_time_advances_pixel)
{
    MutableUint32Source stepTimeMs(1);
    MutableUint32Source color(0x112233);
    ZigZagAnimationDependencies deps(stepTimeMs, color);

    ZigZagAnimation *animation = ZigZagAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    reset_capture();
    animation->tick(&kTestConfig, 2, 0);

    zassert_equal(sCapture.litPixelWrites, 1, "Expected exactly one lit pixel write");
    zassert_equal(sCapture.x, 1, "Expected lit pixel to advance to x=1");
    zassert_equal(sCapture.y, 0, "Expected lit pixel to remain on row 0");
    zassert_equal(sCapture.red, 0x11, "Expected injected red component");
    zassert_equal(sCapture.green, 0x22, "Expected injected green component");
    zassert_equal(sCapture.blue, 0x33, "Expected injected blue component");
}

ZTEST(zigzag_animation_di_tests, test_injected_step_time_holds_pixel_when_not_elapsed)
{
    MutableUint32Source stepTimeMs(1000);
    MutableUint32Source color(0xAA5500);
    ZigZagAnimationDependencies deps(stepTimeMs, color);

    ZigZagAnimation *animation = ZigZagAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    reset_capture();
    animation->tick(&kTestConfig, 1, 0);

    zassert_equal(sCapture.litPixelWrites, 1, "Expected exactly one lit pixel write");
    zassert_equal(sCapture.x, 0, "Expected pixel to remain at x=0 when step time has not elapsed");
    zassert_equal(sCapture.red, 0xAA, "Expected injected red component");
    zassert_equal(sCapture.green, 0x55, "Expected injected green component");
    zassert_equal(sCapture.blue, 0x00, "Expected injected blue component");
}
