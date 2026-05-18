#include <animations/fft_bars_animation.h>

/* Width of each rendered bar in display pixels. */
static constexpr size_t kBarWidthPx = 2;

/* Maps mean bucket power to a bar-height fraction in [0, 1].
 * Tune empirically: kEnergyScale=20 means energy=0.05 fills the display. */
static constexpr float kEnergyScale = 20.0f;

void FftBarsAnimation::setAudioSource(AnimationAudioSource &source)
{
    audioSource_ = &source;
}

void FftBarsAnimation::setColor(const AnimationUint32ParameterSource &color)
{
    color_ = &color;
}

void FftBarsAnimation::init()
{
    for (size_t b = 0; b < kMaxDisplayBuckets; b++)
    {
        smoothed_[b] = 0.0f;
    }
}

void FftBarsAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs)
{
    ARG_UNUSED(timeSinceLastTickMs);

    if (!audioSource_ || !color_)
    {
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

    uint32_t color = color_->get();
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = (color >> 0) & 0xFF;

    size_t numBuckets = audioSource_->numDisplayBuckets();
    if (numBuckets > kMaxDisplayBuckets)
    {
        numBuckets = kMaxDisplayBuckets;
    }

    size_t W = renderer.displayWidth();
    size_t H = renderer.displayHeight();

    /* Update smoothed energies with exponential moving average. */
    for (size_t bucket = 0; bucket < numBuckets; bucket++)
    {
        float energy = audioSource_->getDisplayBucketEnergy(bucket);
        smoothed_[bucket] = 0.3f * energy + 0.7f * smoothed_[bucket];
    }

    /* Render bars: each bucket occupies exactly kBarWidthPx columns. */
    size_t barAreaWidth = numBuckets * kBarWidthPx;

    for (size_t bucket = 0; bucket < numBuckets; bucket++)
    {
        size_t startX = bucket * kBarWidthPx;
        if (startX >= W)
        {
            break;
        }

        float fraction = smoothed_[bucket] * kEnergyScale;
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

        for (size_t bx = 0; bx < kBarWidthPx && startX + bx < W; bx++)
        {
            for (size_t y = 0; y < H; y++)
            {
                /* Bars grow upward from the bottom (y=H-1 is the bottom row). */
                bool lit = (H > 0) && (y >= H - barHeight);
                renderer.setPixel(startX + bx, y, lit ? r : 0, lit ? g : 0, lit ? b : 0);
            }
        }
    }

    /* Black-fill any display columns to the right of the bar area. */
    for (size_t x = barAreaWidth; x < W; x++)
    {
        for (size_t y = 0; y < H; y++)
        {
            renderer.setPixel(x, y, 0, 0, 0);
        }
    }
}
