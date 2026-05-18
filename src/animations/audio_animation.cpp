#include <animations/audio_animation.h>

/* Maximum number of bands this animation handles.
 * Matches AUDIO_NUM_BANDS but kept here to avoid pulling in audio_dsp.h. */
static constexpr size_t kMaxBands = 4;

/* How long (ms) a beat flash stays lit after the beat fires. */
static constexpr size_t kBeatHoldMs = 100;

/* Maps band energy to a bar-height fraction in [0, 1].
 * Tune empirically: kEnergyScale=20 means energy=0.05 fills the display. */
static constexpr float kEnergyScale = 20.0f;

void AudioAnimation::setAudioSource(AnimationAudioSource &source)
{
    audioSource_ = &source;
}

void AudioAnimation::setBtParameters(const AnimationUint32ParameterSource &mode,
                                     const AnimationUint32ParameterSource &color)
{
    mode_ = &mode;
    color_ = &color;
}

void AudioAnimation::init()
{
    for (size_t b = 0; b < kMaxBands; b++)
    {
        smoothed_[b] = 0.0f;
        beatHoldMs_[b] = 0;
    }
}

void AudioAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs)
{
    if (!audioSource_ || !mode_ || !color_)
    {
        /* Render blank frame until all dependencies are bound. */
        for (size_t x = 0; x < renderer.displayWidth(); x++)
        {
            for (size_t y = 0; y < renderer.displayHeight(); y++)
            {
                renderer.setPixel(x, y, 0, 0, 0);
            }
        }
        return;
    }

    audioSource_->update();

    auto mode = static_cast<AudioAnimationMode>(mode_->get());
    switch (mode)
    {
    case AudioAnimationMode::FrequencyBars:
        tickFrequencyBars(renderer);
        break;
    case AudioAnimationMode::BeatColor:
    default:
        tickBeatColor(renderer, timeSinceLastTickMs);
        break;
    }
}

void AudioAnimation::tickBeatColor(AnimationRenderer &renderer, size_t timeSinceLastTickMs)
{
    uint32_t color = color_->get();
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = (color >> 0) & 0xFF;

    size_t numBands = audioSource_->numBands();
    if (numBands > kMaxBands)
    {
        numBands = kMaxBands;
    }

    /* Update per-band beat-hold timers. */
    for (size_t band = 0; band < numBands; band++)
    {
        if (audioSource_->isBeat(band))
        {
            beatHoldMs_[band] = kBeatHoldMs;
        }
        else if (timeSinceLastTickMs >= beatHoldMs_[band])
        {
            beatHoldMs_[band] = 0;
        }
        else
        {
            beatHoldMs_[band] -= timeSinceLastTickMs;
        }
    }

    size_t W = renderer.displayWidth();
    size_t H = renderer.displayHeight();

    /* Each band occupies an equal-width column strip across the display. */
    size_t stripWidth = (W + numBands - 1) / numBands;

    for (size_t x = 0; x < W; x++)
    {
        size_t band = x / stripWidth;
        if (band >= numBands)
        {
            band = numBands - 1;
        }
        bool lit = (beatHoldMs_[band] > 0);
        for (size_t y = 0; y < H; y++)
        {
            renderer.setPixel(x, y, lit ? r : 0, lit ? g : 0, lit ? b : 0);
        }
    }
}

void AudioAnimation::tickFrequencyBars(AnimationRenderer &renderer)
{
    uint32_t color = color_->get();
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = (color >> 0) & 0xFF;

    size_t numBands = audioSource_->numBands();
    if (numBands > kMaxBands)
    {
        numBands = kMaxBands;
    }

    size_t W = renderer.displayWidth();
    size_t H = renderer.displayHeight();

    /* Update smoothed energies with exponential moving average. */
    for (size_t band = 0; band < numBands; band++)
    {
        float energy = audioSource_->getBandEnergy(band);
        smoothed_[band] = 0.3f * energy + 0.7f * smoothed_[band];
    }

    size_t stripWidth = (W + numBands - 1) / numBands;

    for (size_t x = 0; x < W; x++)
    {
        size_t band = x / stripWidth;
        if (band >= numBands)
        {
            band = numBands - 1;
        }

        float fraction = smoothed_[band] * kEnergyScale;
        if (fraction > 1.0f)
        {
            fraction = 1.0f;
        }
        if (fraction < 0.0f)
        {
            fraction = 0.0f;
        }

        /* Add 0.5 for rounding. */
        size_t barHeight = static_cast<size_t>(fraction * static_cast<float>(H) + 0.5f);

        for (size_t y = 0; y < H; y++)
        {
            /* Bars grow upward from the bottom (y=H-1 is the bottom row). */
            bool lit = (H > 0) && (y >= H - barHeight);
            renderer.setPixel(x, y, lit ? r : 0, lit ? g : 0, lit ? b : 0);
        }
    }
}
