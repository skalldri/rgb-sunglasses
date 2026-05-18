#include <animations/fft_bars_animation.h>

/* Width of each rendered bar in display pixels. */
static constexpr size_t kBarWidthPx = 2;

/* Maps mean bucket power to a bar-height fraction in [0, 1].
 * Tune empirically: kEnergyScale=20 means energy=0.05 fills the display. */
static constexpr float kEnergyScale = 20.0f;

/* Exponential moving average: weight applied to the newest energy sample.
 * The complementary weight (1 - kSmoothingCoeff) is applied to the previous
 * smoothed value.  Lower = slower/smoother, higher = faster/more reactive. */
static constexpr float kSmoothingCoeff = 0.3f;

/* ── Gradient constants ──────────────────────────────────────────────────────
 * Traditional VU colours: green (bottom, silence) → orange → red (top, clip).
 * Each lit pixel is coloured by its absolute row position so the hue conveys
 * how close the bar is to clipping, independent of per-bar height. */
struct GradientStop { uint8_t r, g, b; };

static constexpr GradientStop kColorGreen  = {   0, 255,   0 };
static constexpr GradientStop kColorOrange = { 255, 165,   0 };
static constexpr GradientStop kColorRed    = { 255,   0,   0 };

/* Fraction of the display height at which the gradient transitions from the
 * green→orange segment to the orange→red segment.  0.5 = halfway up. */
static constexpr float kGradientMidpoint = 0.5f;

/* Linearly interpolate a single colour channel. */
static uint8_t lerp_channel(uint8_t from, uint8_t to, float t)
{
    return static_cast<uint8_t>(from + t * (static_cast<int>(to) - static_cast<int>(from)) + 0.5f);
}

/* Map a row-position fraction [0=bottom, 1=top] to an RGB gradient colour. */
static void gradient_color(float fraction, uint8_t &r, uint8_t &g, uint8_t &b)
{
    GradientStop from, to;
    float t;

    if (fraction < kGradientMidpoint)
    {
        from = kColorGreen;
        to   = kColorOrange;
        t    = fraction / kGradientMidpoint;
    }
    else
    {
        from = kColorOrange;
        to   = kColorRed;
        t    = (fraction - kGradientMidpoint) / (1.0f - kGradientMidpoint);
    }

    r = lerp_channel(from.r, to.r, t);
    g = lerp_channel(from.g, to.g, t);
    b = lerp_channel(from.b, to.b, t);
}

void FftBarsAnimation::setAudioSource(AnimationAudioSource &source)
{
    audioSource_ = &source;
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

    if (!audioSource_)
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
        smoothed_[bucket] = kSmoothingCoeff * energy + (1.0f - kSmoothingCoeff) * smoothed_[bucket];
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

        size_t barHeight = static_cast<size_t>(fraction * static_cast<float>(H) + 0.5f);

        for (size_t bx = 0; bx < kBarWidthPx && startX + bx < W; bx++)
        {
            for (size_t y = 0; y < H; y++)
            {
                bool lit = (H > 0) && (y >= H - barHeight);
                if (lit)
                {
                    /* Colour depends on absolute row: bottom = green, top = red. */
                    float rowFraction = (H > 1)
                                            ? static_cast<float>(H - 1 - y) /
                                                  static_cast<float>(H - 1)
                                            : 0.0f;
                    uint8_t r, g, b;
                    gradient_color(rowFraction, r, g, b);
                    renderer.setPixel(startX + bx, y, r, g, b);
                }
                else
                {
                    renderer.setPixel(startX + bx, y, 0, 0, 0);
                }
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
