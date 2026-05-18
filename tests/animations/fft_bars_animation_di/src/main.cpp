#include <zephyr/ztest.h>

#include <animations/fft_bars_animation.h>
#include <animations/animation_renderer.h>

namespace
{
    /* 4 test display buckets.
     * Test display is 8 px wide; halfWidth=4, kBarWidthPx=1 → 4 buckets per side.
     * Left:  bucket 0 → x=0, bucket 1 → x=1, bucket 2 → x=2, bucket 3 → x=3
     * Right (mirror): bucket 3 → x=4, bucket 2 → x=5, bucket 1 → x=6, bucket 0 → x=7 */
    class MutableAudioSource : public AnimationAudioSource
    {
    public:
        void update() override {}

        /* Beat bands — not used by FftBarsAnimation, return stubs. */
        size_t numBands() const override { return 0; }
        float getBandEnergy(size_t) const override { return 0.0f; }
        bool isBeat(size_t) const override { return false; }

        size_t numDisplayBuckets() const override { return kTestBuckets; }

        float getDisplayBucketEnergy(size_t bucket) const override
        {
            return (bucket < kTestBuckets) ? energy_[bucket] : 0.0f;
        }

        void setEnergy(size_t bucket, float energy)
        {
            if (bucket < kTestBuckets) energy_[bucket] = energy;
        }

        void resetAll()
        {
            for (size_t b = 0; b < kTestBuckets; b++) energy_[b] = 0.0f;
        }

        /* 4 buckets × 1 px × 2 sides = 8 px — fills the test display exactly. */
        static constexpr size_t kTestBuckets = 4;

    private:
        float energy_[kTestBuckets] = {};
    };

    /* 8 × 4 display. */
    static constexpr size_t kTestWidth  = 8;
    static constexpr size_t kTestHeight = 4;

    struct PixelColor
    {
        uint8_t r = 0, g = 0, b = 0;
        bool isBlack() const { return r == 0 && g == 0 && b == 0; }
    };

    PixelColor sPixels[kTestWidth][kTestHeight];

    class CapturingTestRenderer : public AnimationRenderer
    {
    public:
        size_t displayWidth()  const override { return kTestWidth; }
        size_t displayHeight() const override { return kTestHeight; }
        void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override
        {
            if (x < kTestWidth && y < kTestHeight)
            {
                sPixels[x][y] = {r, g, b};
            }
        }
    };

    void resetCapture()
    {
        for (size_t x = 0; x < kTestWidth; x++)
        {
            for (size_t y = 0; y < kTestHeight; y++)
            {
                sPixels[x][y] = {};
            }
        }
    }

    bool allPixelsDark()
    {
        for (size_t x = 0; x < kTestWidth; x++)
        {
            for (size_t y = 0; y < kTestHeight; y++)
            {
                if (!sPixels[x][y].isBlack()) return false;
            }
        }
        return true;
    }

    /* With kBarWidthPx=1: left column = bucket, right mirror = (W-1-bucket). */
    bool bucketIsDarkBothSides(size_t bucket)
    {
        size_t leftX  = bucket;
        size_t rightX = kTestWidth - 1 - bucket;
        for (size_t y = 0; y < kTestHeight; y++)
        {
            if (!sPixels[leftX][y].isBlack())  return false;
            if (!sPixels[rightX][y].isBlack()) return false;
        }
        return true;
    }
}

ZTEST_SUITE(fft_bars_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

/* Zero energy → entire display dark. */
ZTEST(fft_bars_animation_di_tests, test_zero_energy_is_dark)
{
    MutableAudioSource audio;
    audio.resetAll();

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    zassert_true(allPixelsDark(), "Display should be fully dark with zero energy");
}

/* Non-zero energy for bucket 0 lights the bottom pixel of both its left column
 * (x=0) and its right mirror column (x=7). */
ZTEST(fft_bars_animation_di_tests, test_energy_lights_both_mirror_sides)
{
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(0, 0.1f);

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    size_t bottomRow = kTestHeight - 1;
    zassert_false(sPixels[0][bottomRow].isBlack(), "Left x=0 bottom should be lit");
    zassert_false(sPixels[7][bottomRow].isBlack(), "Right mirror x=7 bottom should be lit");
}

/* Energy only in bucket 2 lights x=2 and its mirror x=5; others stay dark. */
ZTEST(fft_bars_animation_di_tests, test_bucket_column_isolation)
{
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(2, 0.1f);

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    size_t bottomRow = kTestHeight - 1;
    zassert_false(sPixels[2][bottomRow].isBlack(), "Left x=2 bottom should be lit");
    zassert_false(sPixels[5][bottomRow].isBlack(), "Right mirror x=5 bottom should be lit");

    zassert_true(bucketIsDarkBothSides(0), "Bucket 0 (both sides) should be dark");
    zassert_true(bucketIsDarkBothSides(1), "Bucket 1 (both sides) should be dark");
    zassert_true(bucketIsDarkBothSides(3), "Bucket 3 (both sides) should be dark");
}

/* Bottom lit pixel is greenish; top lit pixel is reddish; right mirror matches left. */
ZTEST(fft_bars_animation_di_tests, test_gradient_bottom_green_top_red)
{
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(0, 1.0f);

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;
    for (int i = 0; i < 20; i++)
    {
        resetCapture();
        anim->tick(renderer, 16);
    }

    size_t bottomRow = kTestHeight - 1;
    zassert_equal(sPixels[0][bottomRow].r, 0,   "Bottom pixel red should be 0");
    zassert_equal(sPixels[0][bottomRow].b, 0,   "Bottom pixel blue should be 0");
    zassert_true(sPixels[0][bottomRow].g > 200, "Bottom pixel should be greenish");

    zassert_equal(sPixels[0][0].r, 255, "Top pixel red should be 255");
    zassert_equal(sPixels[0][0].g, 0,   "Top pixel green should be 0");
    zassert_equal(sPixels[0][0].b, 0,   "Top pixel blue should be 0");

    /* Right mirror must match left. */
    zassert_equal(sPixels[7][bottomRow].r, sPixels[0][bottomRow].r,
                  "Right mirror bottom should match left");
    zassert_equal(sPixels[7][0].r, sPixels[0][0].r,
                  "Right mirror top should match left");
}

/* init() resets smoothed energy; display returns to dark immediately. */
ZTEST(fft_bars_animation_di_tests, test_init_resets_smoothed_energy)
{
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(0, 0.1f);

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;
    for (int i = 0; i < 10; i++)
    {
        resetCapture();
        anim->tick(renderer, 16);
    }
    zassert_false(sPixels[0][kTestHeight - 1].isBlack(),
                  "Bottom pixel should be lit after energy builds up");

    audio.setEnergy(0, 0.0f);
    anim->init();

    resetCapture();
    anim->tick(renderer, 16);
    zassert_true(allPixelsDark(),
                 "Display should be dark immediately after init() with zero energy");
}
