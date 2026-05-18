#include <zephyr/ztest.h>

#include <animations/audio_animation.h>
#include <animations/animation_renderer.h>

namespace
{
    /* Controllable uint32 parameter source used for mode and color injection. */
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

    /* Controllable audio source for injecting FFT data without the real queue. */
    class MutableAudioSource : public AnimationAudioSource
    {
    public:
        /* update() is a no-op in tests: values are set directly via the setters below. */
        void update() override {}

        float getBandEnergy(size_t band) const override
        {
            return (band < kTestBands) ? energy_[band] : 0.0f;
        }

        bool isBeat(size_t band) const override
        {
            return (band < kTestBands) && beat_[band];
        }

        size_t numBands() const override
        {
            return kTestBands;
        }

        void setEnergy(size_t band, float energy)
        {
            if (band < kTestBands)
            {
                energy_[band] = energy;
            }
        }

        void setBeat(size_t band, bool beat)
        {
            if (band < kTestBands)
            {
                beat_[band] = beat;
            }
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

    /* Test display: 8 columns × 4 rows gives 4 bands of 2 pixels each. */
    static constexpr size_t kTestWidth = 8;
    static constexpr size_t kTestHeight = 4;

    struct PixelColor
    {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;

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

    /* Returns true if every pixel in the column strip for `band` is non-black. */
    bool bandStripIsLit(size_t band)
    {
        /* Strip width = ceil(8/4) = 2. Band b occupies x=[b*2, b*2+1]. */
        size_t startX = band * 2;
        size_t endX = startX + 2;
        for (size_t x = startX; x < endX && x < kTestWidth; x++)
        {
            for (size_t y = 0; y < kTestHeight; y++)
            {
                if (sPixels[x][y].isBlack())
                {
                    return false;
                }
            }
        }
        return true;
    }

    /* Returns true if every pixel in the column strip for `band` is black. */
    bool bandStripIsDark(size_t band)
    {
        size_t startX = band * 2;
        size_t endX = startX + 2;
        for (size_t x = startX; x < endX && x < kTestWidth; x++)
        {
            for (size_t y = 0; y < kTestHeight; y++)
            {
                if (!sPixels[x][y].isBlack())
                {
                    return false;
                }
            }
        }
        return true;
    }

    /* Returns true if every pixel across the whole display is black. */
    bool allPixelsDark()
    {
        for (size_t x = 0; x < kTestWidth; x++)
        {
            for (size_t y = 0; y < kTestHeight; y++)
            {
                if (!sPixels[x][y].isBlack())
                {
                    return false;
                }
            }
        }
        return true;
    }
}

ZTEST_SUITE(audio_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

/* BeatColor mode: a beat on band 0 lights up band 0's column strip. */
ZTEST(audio_animation_di_tests, test_beat_color_beat_lights_band_strip)
{
    MutableUint32Source mode(static_cast<uint32_t>(AudioAnimationMode::BeatColor));
    MutableUint32Source color(0x00FF0000); /* red */

    MutableAudioSource audio;
    audio.resetAll();
    audio.setBeat(0, true);

    AudioAnimation *anim = AudioAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setBtParameters(mode, color);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    zassert_true(bandStripIsLit(0), "Band 0 strip should be lit on beat");
    zassert_true(bandStripIsDark(1), "Band 1 strip should be dark (no beat)");
    zassert_true(bandStripIsDark(2), "Band 2 strip should be dark (no beat)");
    zassert_true(bandStripIsDark(3), "Band 3 strip should be dark (no beat)");
}

/* BeatColor mode: no beats → entire display is dark. */
ZTEST(audio_animation_di_tests, test_beat_color_no_beat_is_dark)
{
    MutableUint32Source mode(static_cast<uint32_t>(AudioAnimationMode::BeatColor));
    MutableUint32Source color(0x00FFFFFF);

    MutableAudioSource audio;
    audio.resetAll();

    AudioAnimation *anim = AudioAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setBtParameters(mode, color);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    zassert_true(allPixelsDark(), "Display should be dark when no beats fire");
}

/* BeatColor mode: beat flash persists for kBeatHoldMs then fades. */
ZTEST(audio_animation_di_tests, test_beat_color_hold_timer)
{
    MutableUint32Source mode(static_cast<uint32_t>(AudioAnimationMode::BeatColor));
    MutableUint32Source color(0x000000FF); /* blue */

    MutableAudioSource audio;
    audio.resetAll();
    audio.setBeat(0, true); /* fire a beat */

    AudioAnimation *anim = AudioAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setBtParameters(mode, color);
    anim->init();

    CapturingTestRenderer renderer;

    /* Tick 1: beat fires, strip should light up. */
    resetCapture();
    anim->tick(renderer, 16);
    zassert_true(bandStripIsLit(0), "Band 0 strip should be lit immediately after beat");

    /* Clear the beat flag; hold timer should keep the strip lit. */
    audio.setBeat(0, false);

    /* Tick 2: 50 ms elapsed, hold timer still has 50 ms left (100 - 50). */
    resetCapture();
    anim->tick(renderer, 50);
    zassert_true(bandStripIsLit(0), "Band 0 strip should stay lit during hold period");

    /* Tick 3: another 60 ms; total 110 ms > 100 ms hold → timer expires. */
    resetCapture();
    anim->tick(renderer, 60);
    zassert_true(bandStripIsDark(0), "Band 0 strip should go dark after hold timer expires");
}

/* FrequencyBars mode: non-zero energy for band 0 lights bottom pixels of its strip. */
ZTEST(audio_animation_di_tests, test_freq_bars_energy_sets_bar_height)
{
    MutableUint32Source mode(static_cast<uint32_t>(AudioAnimationMode::FrequencyBars));
    MutableUint32Source color(0x0000FF00); /* green */

    MutableAudioSource audio;
    audio.resetAll();
    /* Energy high enough that even after one smoothing step the bottom pixel lights up.
     * With kEnergyScale=20 and height=4: energy=0.1 → fraction=1.0 → barHeight=4 (full). */
    audio.setEnergy(0, 0.1f);

    AudioAnimation *anim = AudioAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setBtParameters(mode, color);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    /* Bottom pixel (y = kTestHeight-1) of band 0 strip must be lit. */
    size_t bottomRow = kTestHeight - 1;
    zassert_false(sPixels[0][bottomRow].isBlack(),
                  "Bottom pixel of band 0 strip should be lit with energy=0.1");
    zassert_false(sPixels[1][bottomRow].isBlack(),
                  "Bottom pixel of band 0 strip (x=1) should be lit with energy=0.1");
}

/* FrequencyBars mode: zero energy for all bands → entire display is dark. */
ZTEST(audio_animation_di_tests, test_freq_bars_zero_energy_is_dark)
{
    MutableUint32Source mode(static_cast<uint32_t>(AudioAnimationMode::FrequencyBars));
    MutableUint32Source color(0x00FFFFFF);

    MutableAudioSource audio;
    audio.resetAll();

    AudioAnimation *anim = AudioAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setBtParameters(mode, color);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    zassert_true(allPixelsDark(), "Display should be fully dark with zero energy");
}

/* FrequencyBars mode: energy only in band 2 lights only band 2's column strip. */
ZTEST(audio_animation_di_tests, test_freq_bars_band_column_isolation)
{
    MutableUint32Source mode(static_cast<uint32_t>(AudioAnimationMode::FrequencyBars));
    MutableUint32Source color(0x00FF00FF); /* magenta */

    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(2, 0.1f); /* only band 2 has energy */

    AudioAnimation *anim = AudioAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setBtParameters(mode, color);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    size_t bottomRow = kTestHeight - 1;
    /* Band 2 strip (x=4, x=5) should be lit at the bottom. */
    zassert_false(sPixels[4][bottomRow].isBlack(), "Band 2 strip x=4 should be lit");
    zassert_false(sPixels[5][bottomRow].isBlack(), "Band 2 strip x=5 should be lit");

    /* Other bands (0, 1, 3) should be dark. */
    zassert_true(bandStripIsDark(0), "Band 0 strip should be dark");
    zassert_true(bandStripIsDark(1), "Band 1 strip should be dark");
    zassert_true(bandStripIsDark(3), "Band 3 strip should be dark");
}

/* Mode switch: changing from FrequencyBars to BeatColor with no beats → dark display. */
ZTEST(audio_animation_di_tests, test_mode_switch_freq_bars_to_beat_color)
{
    MutableUint32Source mode(static_cast<uint32_t>(AudioAnimationMode::FrequencyBars));
    MutableUint32Source color(0x00FFFFFF);

    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(0, 0.1f);

    AudioAnimation *anim = AudioAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setBtParameters(mode, color);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);
    zassert_false(allPixelsDark(), "FrequencyBars with energy should produce lit pixels");

    /* Switch to BeatColor with no beats → display goes dark. */
    mode.set(static_cast<uint32_t>(AudioAnimationMode::BeatColor));
    audio.setEnergy(0, 0.0f);
    anim->init(); /* reset hold timers */

    resetCapture();
    anim->tick(renderer, 16);
    zassert_true(allPixelsDark(), "BeatColor with no beats should produce a dark display");
}
