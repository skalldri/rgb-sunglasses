#include <zephyr/ztest.h>

#define private public
#include <animations/bt_animations.h>
#undef private

#include <animations/animation_renderer.h>

namespace
{
    static constexpr size_t kWidth = 40;
    static constexpr size_t kHeight = 12;

    struct PixelState
    {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
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
                sPixels[x][y] = {};
            }
        }
    }

    uint8_t blue_at(size_t x, size_t y) { return sPixels[x][y].b; }
}

ZTEST_SUITE(bt_animations_di_tests, NULL, NULL, NULL, NULL, NULL);

// --- BtAdvertisingAnimation ---

ZTEST(bt_animations_di_tests, test_advertising_tick_fills_all_pixels_blue)
{
    BtAdvertisingAnimation *anim = BtAdvertisingAnimation::getInstance();
    anim->init();

    CapturingRenderer renderer;
    reset_pixels();
    anim->tick(renderer, 1);

    for (size_t x = 0; x < kWidth; x++)
    {
        for (size_t y = 0; y < kHeight; y++)
        {
            zassert_equal(sPixels[x][y].r, 0, "Expected r=0 at (%d,%d)", x, y);
            zassert_equal(sPixels[x][y].g, 0, "Expected g=0 at (%d,%d)", x, y);
            zassert_true(sPixels[x][y].b > 0, "Expected b>0 at (%d,%d)", x, y);
        }
    }
}

ZTEST(bt_animations_di_tests, test_advertising_brightness_is_minimum_at_start)
{
    BtAdvertisingAnimation *anim = BtAdvertisingAnimation::getInstance();
    anim->init();

    CapturingRenderer renderer;
    reset_pixels();
    // 1ms into the cycle: minimum brightness
    anim->tick(renderer, 1);
    uint8_t brightness_at_1ms = blue_at(0, 0);

    zassert_equal(brightness_at_1ms, BtAdvertisingAnimation::kMinFade,
                  "Expected minimum brightness at start of cycle");
}

ZTEST(bt_animations_di_tests, test_advertising_brightness_is_maximum_at_half_cycle)
{
    BtAdvertisingAnimation *anim = BtAdvertisingAnimation::getInstance();
    anim->init();

    CapturingRenderer renderer;
    // Advance to halfway point (500ms)
    anim->tick(renderer, 500);
    reset_pixels();
    anim->tick(renderer, 0);
    uint8_t brightness_at_500ms = blue_at(0, 0);

    zassert_equal(brightness_at_500ms, BtAdvertisingAnimation::kMaxFade,
                  "Expected maximum brightness at half-cycle point");
}

ZTEST(bt_animations_di_tests, test_advertising_brightness_increases_toward_half_cycle)
{
    BtAdvertisingAnimation *anim = BtAdvertisingAnimation::getInstance();
    anim->init();

    CapturingRenderer renderer;
    reset_pixels();
    anim->tick(renderer, 1);
    uint8_t early = blue_at(0, 0);

    anim->init();
    reset_pixels();
    anim->tick(renderer, 499);
    uint8_t late = blue_at(0, 0);

    zassert_true(early < late, "Expected brightness to increase toward half-cycle");
}

// --- BtConnectingAnimation ---

ZTEST(bt_animations_di_tests, test_connecting_tick_starts_dim)
{
    BtConnectingAnimation *anim = BtConnectingAnimation::getInstance();
    anim->init();

    CapturingRenderer renderer;
    reset_pixels();
    anim->tick(renderer, 1);

    zassert_equal(blue_at(0, 0), BtConnectingAnimation::kMinFlash,
                  "Expected dim brightness before first flash transition");
}

ZTEST(bt_animations_di_tests, test_connecting_tick_flips_bright_after_flash_speed)
{
    BtConnectingAnimation *anim = BtConnectingAnimation::getInstance();
    anim->init();

    CapturingRenderer renderer;
    // Tick past kFlashSpeedMs (300ms) to trigger the flip
    anim->tick(renderer, 301);
    reset_pixels();
    anim->tick(renderer, 0);

    zassert_equal(blue_at(0, 0), BtConnectingAnimation::kMaxFlash,
                  "Expected maximum brightness after flash speed elapsed");
}

ZTEST(bt_animations_di_tests, test_connecting_tick_does_not_flip_at_exact_boundary)
{
    BtConnectingAnimation *anim = BtConnectingAnimation::getInstance();
    anim->init();

    CapturingRenderer renderer;
    // Tick exactly kFlashSpeedMs — condition is >, so no flip yet
    anim->tick(renderer, 300);
    reset_pixels();
    anim->tick(renderer, 0);

    zassert_equal(blue_at(0, 0), BtConnectingAnimation::kMinFlash,
                  "Expected still dim at exactly kFlashSpeedMs (condition is >)");
}

// --- BtPairingAnimation ---

ZTEST(bt_animations_di_tests, test_pairing_offset_advances_after_step_time_elapses)
{
    BtPairingAnimation *anim = BtPairingAnimation::getInstance();
    anim->setPairingCode(123456);
    anim->init();

    zassert_equal(anim->currentTextOffset, 0, "Expected offset to start at 0 after init");

    CapturingRenderer renderer;
    // Tick past kStepTimeMs (100ms)
    anim->tick(renderer, 101);

    zassert_equal(anim->currentTextOffset, -1, "Expected offset to decrement after step time elapses");
}

ZTEST(bt_animations_di_tests, test_pairing_offset_does_not_advance_before_step_time)
{
    BtPairingAnimation *anim = BtPairingAnimation::getInstance();
    anim->setPairingCode(123456);
    anim->init();

    CapturingRenderer renderer;
    // Tick for exactly kStepTimeMs — condition is >, so no advance
    anim->tick(renderer, 100);

    zassert_equal(anim->currentTextOffset, 0,
                  "Expected offset unchanged at exactly kStepTimeMs (condition is >)");
}
