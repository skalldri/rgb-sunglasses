#include <zephyr/ztest.h>

#include <animations/null_animation.h>
#include <animations/animation_renderer.h>

namespace
{
    static constexpr size_t kWidth = 8;
    static constexpr size_t kHeight = 4;

    struct PixelState
    {
        uint8_t r = 1;
        uint8_t g = 1;
        uint8_t b = 1;
    };

    PixelState sPixels[kWidth][kHeight];

    class CapturingRenderer : public AnimationRenderer
    {
    public:
        size_t displayWidth() const override { return kWidth; }
        size_t displayHeight() const override { return kHeight; }
        void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override
        {
            if (x < kWidth && y < kHeight)
            {
                sPixels[x][y] = {r, g, b};
            }
        }
    };

    void reset_pixels()
    {
        for (size_t x = 0; x < kWidth; x++)
        {
            for (size_t y = 0; y < kHeight; y++)
            {
                sPixels[x][y] = {1, 1, 1};
            }
        }
    }
}

ZTEST_SUITE(null_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(null_animation_di_tests, test_tick_writes_all_pixels_off)
{
    NullAnimation *animation = NullAnimation::getInstance();
    animation->init();

    CapturingRenderer renderer;
    reset_pixels();
    animation->tick(renderer, 10);

    for (size_t x = 0; x < kWidth; x++)
    {
        for (size_t y = 0; y < kHeight; y++)
        {
            zassert_equal(sPixels[x][y].r, 0, "Expected r=0 at (%d,%d)", x, y);
            zassert_equal(sPixels[x][y].g, 0, "Expected g=0 at (%d,%d)", x, y);
            zassert_equal(sPixels[x][y].b, 0, "Expected b=0 at (%d,%d)", x, y);
        }
    }
}
