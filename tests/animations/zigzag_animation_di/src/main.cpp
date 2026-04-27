#include <zephyr/ztest.h>

#include <animations/zigzag_animation.h>
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

    class CapturingTestRenderer : public AnimationRenderer
    {
    public:
        size_t displayWidth() const override { return 2; }
        size_t displayHeight() const override { return 1; }
        void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override
        {
            if (r != 0 || g != 0 || b != 0)
            {
                sCapture.x = x;
                sCapture.y = y;
                sCapture.red = r;
                sCapture.green = g;
                sCapture.blue = b;
                sCapture.litPixelWrites++;
            }
        }
    };

    void reset_capture()
    {
        sCapture = {};
    }
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

ZTEST(zigzag_animation_di_tests, test_injected_step_time_holds_pixel_when_not_elapsed)
{
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
