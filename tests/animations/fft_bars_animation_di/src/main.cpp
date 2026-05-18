#include <zephyr/ztest.h>

#include <animations/fft_bars_animation.h>
#include <animations/animation_renderer.h>

namespace
{
    /* 4 test display buckets: with kBarWidthPx=2 and an 8-wide renderer this
     * fills exactly 4*2=8 px — the full test display width. */
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

        static constexpr size_t kTestBuckets = 4;

    private:
        float energy_[kTestBuckets] = {};
    };

    /* Test display: 8 columns × 4 rows. 4 test buckets × 2 px/bar = 8 px wide. */
    static constexpr size_t kTestWidth = 8;
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
        size_t displayWidth() const override { return kTestWidth; }
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

    /* Each bucket occupies exactly kBarWidthPx=2 columns.
     * Bucket b → x = [b*2, b*2+1]. */
    bool bucketStripIsDark(size_t bucket)
    {
        size_t startX = bucket * 2;
        for (size_t x = startX; x < startX + 2 && x < kTestWidth; x++)
        {
            for (size_t y = 0; y < kTestHeight; y++)
            {
                if (!sPixels[x][y].isBlack()) return false;
            }
        }
        return true;
    }
}

ZTEST_SUITE(fft_bars_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

/* Zero energy for all buckets → entire display is dark. */
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

/* Non-zero energy for bucket 0 lights the bottom pixel of its 2-px-wide strip. */
ZTEST(fft_bars_animation_di_tests, test_energy_lights_bottom_pixel)
{
    MutableAudioSource audio;
    audio.resetAll();
    /* kEnergyScale=20: energy=0.1 → fraction=1.0 after smoothing converges.
     * After one tick: smoothed = 0.3*0.1 = 0.03 → fraction=0.6 → barHeight >= 1. */
    audio.setEnergy(0, 0.1f);

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    size_t bottomRow = kTestHeight - 1;
    zassert_false(sPixels[0][bottomRow].isBlack(),
                  "Bottom pixel of bucket 0 strip (x=0) should be lit");
    zassert_false(sPixels[1][bottomRow].isBlack(),
                  "Bottom pixel of bucket 0 strip (x=1) should be lit");
}

/* Energy only in bucket 2 lights only bucket 2's 2-px column strip. */
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
    /* Bucket 2 → x = [4, 5] */
    zassert_false(sPixels[4][bottomRow].isBlack(), "Bucket 2 strip x=4 should be lit");
    zassert_false(sPixels[5][bottomRow].isBlack(), "Bucket 2 strip x=5 should be lit");

    zassert_true(bucketStripIsDark(0), "Bucket 0 strip should be dark");
    zassert_true(bucketStripIsDark(1), "Bucket 1 strip should be dark");
    zassert_true(bucketStripIsDark(3), "Bucket 3 strip should be dark");
}

/* Bottom lit pixel is greenish; top lit pixel is reddish (gradient test).
 * Use full energy so the bar fills the entire display height. */
ZTEST(fft_bars_animation_di_tests, test_gradient_bottom_green_top_red)
{
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(0, 1.0f); /* saturate: will converge to full bar */

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;

    /* Drive smoothing to convergence. */
    for (int i = 0; i < 20; i++)
    {
        resetCapture();
        anim->tick(renderer, 16);
    }

    /* Bottom row: fraction=0.0 → pure green (r=0, g=255, b=0). */
    size_t bottomRow = kTestHeight - 1;
    zassert_equal(sPixels[0][bottomRow].r, 0,   "Bottom pixel red should be 0");
    zassert_equal(sPixels[0][bottomRow].b, 0,   "Bottom pixel blue should be 0");
    zassert_true(sPixels[0][bottomRow].g > 200, "Bottom pixel should be greenish");

    /* Top row: fraction=1.0 → pure red (r=255, g=0, b=0). */
    size_t topRow = 0;
    zassert_equal(sPixels[0][topRow].r, 255, "Top pixel red should be 255");
    zassert_equal(sPixels[0][topRow].g, 0,   "Top pixel green should be 0");
    zassert_equal(sPixels[0][topRow].b, 0,   "Top pixel blue should be 0");
}

/* init() resets the smoothed energy so the animation starts from silence. */
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
