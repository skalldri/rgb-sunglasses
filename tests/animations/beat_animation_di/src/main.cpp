#include <zephyr/ztest.h>

#include <animations/beat_animation.h>
#include <animations/animation_renderer.h>

namespace
{
    class MutableUint32Source : public AnimationUint32ParameterSource
    {
    public:
        explicit MutableUint32Source(uint32_t value) : value_(value) {}
        uint32_t get() const override { return value_; }
        void set(uint32_t value) { value_ = value; }
    private:
        uint32_t value_;
    };

    class MutableAudioSource : public AnimationAudioSource
    {
    public:
        void update() override {}

        float getBandEnergy(size_t band) const override
        {
            return (band < kTestBands) ? energy_[band] : 0.0f;
        }

        bool isBeat(size_t band) const override
        {
            return (band < kTestBands) && beat_[band];
        }

        size_t numBands() const override { return kTestBands; }

        /* Display buckets are not used by BeatAnimation; return stubs. */
        size_t numDisplayBuckets() const override { return 0; }
        float getDisplayBucketEnergy(size_t) const override { return 0.0f; }

        void setBeat(size_t band, bool beat)
        {
            if (band < kTestBands) beat_[band] = beat;
        }

        void resetAll()
        {
            for (size_t b = 0; b < kTestBands; b++)
            {
                energy_[b] = 0.0f;
                beat_[b] = false;
            }
        }

        static constexpr size_t kTestBands = 4;

    private:
        float energy_[kTestBands] = {};
        bool beat_[kTestBands] = {};
    };

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

    /* Strip width = ceil(8/4) = 2. Band b occupies x = [b*2, b*2+1]. */
    bool bandStripIsLit(size_t band)
    {
        size_t startX = band * 2;
        for (size_t x = startX; x < startX + 2 && x < kTestWidth; x++)
        {
            for (size_t y = 0; y < kTestHeight; y++)
            {
                if (sPixels[x][y].isBlack()) return false;
            }
        }
        return true;
    }

    bool bandStripIsDark(size_t band)
    {
        size_t startX = band * 2;
        for (size_t x = startX; x < startX + 2 && x < kTestWidth; x++)
        {
            for (size_t y = 0; y < kTestHeight; y++)
            {
                if (!sPixels[x][y].isBlack()) return false;
            }
        }
        return true;
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
}

ZTEST_SUITE(beat_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

/* A beat on band 0 lights up band 0's column strip; others stay dark. */
ZTEST(beat_animation_di_tests, test_beat_lights_band_strip)
{
    MutableUint32Source color(0x00FF0000); /* red */
    MutableAudioSource audio;
    audio.resetAll();
    audio.setBeat(0, true);

    BeatAnimation *anim = BeatAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setColor(color);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    zassert_true(bandStripIsLit(0),  "Band 0 strip should be lit on beat");
    zassert_true(bandStripIsDark(1), "Band 1 strip should be dark (no beat)");
    zassert_true(bandStripIsDark(2), "Band 2 strip should be dark (no beat)");
    zassert_true(bandStripIsDark(3), "Band 3 strip should be dark (no beat)");
}

/* No beats → entire display is dark. */
ZTEST(beat_animation_di_tests, test_no_beat_is_dark)
{
    MutableUint32Source color(0x00FFFFFF);
    MutableAudioSource audio;
    audio.resetAll();

    BeatAnimation *anim = BeatAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setColor(color);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    zassert_true(allPixelsDark(), "Display should be dark when no beats fire");
}

/* Beat flash persists for ~100 ms then fades. */
ZTEST(beat_animation_di_tests, test_beat_hold_timer)
{
    MutableUint32Source color(0x000000FF); /* blue */
    MutableAudioSource audio;
    audio.resetAll();
    audio.setBeat(0, true);

    BeatAnimation *anim = BeatAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setColor(color);
    anim->init();

    CapturingTestRenderer renderer;

    /* Tick 1: beat fires → strip lit. */
    resetCapture();
    anim->tick(renderer, 16);
    zassert_true(bandStripIsLit(0), "Band 0 strip should be lit immediately after beat");

    audio.setBeat(0, false);

    /* Tick 2: 50 ms later, hold timer has 50 ms remaining → still lit. */
    resetCapture();
    anim->tick(renderer, 50);
    zassert_true(bandStripIsLit(0), "Band 0 strip should stay lit during hold period");

    /* Tick 3: another 60 ms; 60 > 50 remaining → timer expires, strip dark. */
    resetCapture();
    anim->tick(renderer, 60);
    zassert_true(bandStripIsDark(0), "Band 0 strip should go dark after hold timer expires");
}

/* Color injected via the parameter source is reflected in lit pixels. */
ZTEST(beat_animation_di_tests, test_injected_color_used)
{
    MutableUint32Source color(0x00FF0000); /* red */
    MutableAudioSource audio;
    audio.resetAll();
    audio.setBeat(0, true);

    BeatAnimation *anim = BeatAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setColor(color);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    zassert_equal(sPixels[0][0].r, 0xFF, "Red channel should match injected color");
    zassert_equal(sPixels[0][0].g, 0x00, "Green channel should be zero for red color");
    zassert_equal(sPixels[0][0].b, 0x00, "Blue channel should be zero for red color");
}
