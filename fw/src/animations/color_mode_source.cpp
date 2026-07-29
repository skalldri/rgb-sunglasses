#include <animations/color_mode_source.h>

namespace {

// Full hue span in the Q16 fixed-point phase accumulator.
constexpr uint32_t kHueSpanQ16 = 1536u << 16;

// SpectrumSweep full-cycle period: speed 255 -> 2 s, speed 0 -> ~60.1 s (linear).
constexpr uint32_t sweep_period_ms(uint8_t speed) {
    return 2000u + (255u - speed) * 228u;
}

// RandomTimerFade pick interval: speed 255 -> 1 s, speed 0 -> ~30.1 s (linear).
constexpr uint32_t timer_interval_ms(uint8_t speed) {
    return 1000u + (255u - speed) * 114u;
}

// Per-channel integer lerp between two 0x00RRGGBB colors, t256 in [0, 256].
uint32_t lerp_color(uint32_t a, uint32_t b, uint32_t t256) {
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const int32_t ca = static_cast<int32_t>((a >> shift) & 0xFFu);
        const int32_t cb = static_cast<int32_t>((b >> shift) & 0xFFu);
        const int32_t c = ca + (((cb - ca) * static_cast<int32_t>(t256)) / 256);
        out |= static_cast<uint32_t>(c & 0xFF) << shift;
    }
    return out;
}

}  // namespace

AnimationBeatSource *ColorModeSource::sDefaultBeatSource_;

void ColorModeSource::setDefaultBeatSource(AnimationBeatSource *src) {
    sDefaultBeatSource_ = src;
}

uint32_t anim_color_from_hue(uint16_t hue1536) {
    const uint32_t sector = (hue1536 % 1536u) >> 8;
    const uint32_t ramp = hue1536 & 0xFFu;
    uint32_t r = 0, g = 0, b = 0;
    switch (sector) {
        case 0:  r = 255;        g = ramp;       b = 0;          break;  // R -> Y
        case 1:  r = 255 - ramp; g = 255;        b = 0;          break;  // Y -> G
        case 2:  r = 0;          g = 255;        b = ramp;       break;  // G -> C
        case 3:  r = 0;          g = 255 - ramp; b = 255;        break;  // C -> B
        case 4:  r = ramp;       g = 0;          b = 255;        break;  // B -> M
        default: r = 255;        g = 0;          b = 255 - ramp; break;  // M -> R
    }
    return (r << 16) | (g << 8) | b;
}

uint32_t ColorModeSource::get() const {
    const uint32_t raw = raw_.get();
    const uint8_t modeByte = static_cast<uint8_t>(raw >> 24);

    // Unknown mode values (incl. 0xFF, the persisted pre-feature default) are Static.
    ColorMode mode;
    switch (modeByte) {
        case static_cast<uint8_t>(ColorMode::SpectrumSweep):
        case static_cast<uint8_t>(ColorMode::RandomOnBeat):
        case static_cast<uint8_t>(ColorMode::RandomOnActivate):
        case static_cast<uint8_t>(ColorMode::RandomTimerFade):
            mode = static_cast<ColorMode>(modeByte);
            break;
        default:
            mode = ColorMode::Static;
            break;
    }

    bool reset = resetPending_.exchange(false, std::memory_order_relaxed);
    if (!stateValid_ || modeByte != lastMode_) {
        reset = true;
    }

    if (mode == ColorMode::Static) {
        // Zero mode work in the passthrough path; state re-seeds on the next
        // mode change anyway, so only the bookkeeping fields are kept current.
        stateValid_ = true;
        lastMode_ = modeByte;
        return raw & 0x00FFFFFFu;
    }

    const uint8_t speed = static_cast<uint8_t>(raw >> 16);
    const int64_t now = now_();

    if (reset) {
        stateValid_ = true;
        lastMode_ = modeByte;
        lastNowMs_ = now;
        segmentStartMs_ = now;
        huePhase16_ = 0;
        // Only the random modes consume entropy on reset. Rolls start from the
        // previous hue even across activations/mode changes, so the first color
        // of a new session is still visibly different from the last.
        if (mode != ColorMode::SpectrumSweep) {
            currentHue_ = rollHueFrom(currentHue_);
        }
        if (mode == ColorMode::RandomTimerFade) {
            prevHue_ = currentHue_;
            targetHue_ = rollHueFrom(currentHue_);
        }
    }

    switch (mode) {
        case ColorMode::SpectrumSweep: {
            // Incremental phase advance: changing the speed byte mid-run changes
            // the rate without a hue jump.
            const uint32_t periodMs = sweep_period_ms(speed);
            int64_t dt = now - lastNowMs_;
            if (dt < 0) {
                dt = 0;
            }
            lastNowMs_ = now;
            huePhase16_ = static_cast<uint32_t>(
                (huePhase16_ + (static_cast<uint64_t>(dt) * kHueSpanQ16) / periodMs) %
                kHueSpanQ16);
            return anim_color_from_hue(static_cast<uint16_t>(huePhase16_ >> 16));
        }

        case ColorMode::RandomOnBeat: {
            // No bound beat source (audio disabled / not wired) degrades to
            // RandomOnActivate: the reset above already rolled a color, hold it.
            AnimationBeatSource *beats = beatSource_ ? beatSource_ : sDefaultBeatSource_;
            if (beats != nullptr && beats->consumeBeat()) {
                currentHue_ = rollHueFrom(currentHue_);
            }
            return anim_color_from_hue(currentHue_);
        }

        case ColorMode::RandomOnActivate:
            return anim_color_from_hue(currentHue_);

        case ColorMode::RandomTimerFade: {
            // Continuous motion: lerp from the previous pick to the next across the
            // whole interval, then immediately start toward a fresh pick.
            const uint32_t intervalMs = timer_interval_ms(speed);
            int64_t elapsed = now - segmentStartMs_;
            if (elapsed < 0) {
                elapsed = 0;
            }
            if (static_cast<uint64_t>(elapsed) >= intervalMs) {
                prevHue_ = targetHue_;
                targetHue_ = rollHueFrom(targetHue_);
                currentHue_ = targetHue_;
                segmentStartMs_ = now;
                elapsed = 0;
            }
            const uint32_t t256 =
                static_cast<uint32_t>((static_cast<uint64_t>(elapsed) * 256u) / intervalMs);
            return lerp_color(anim_color_from_hue(prevHue_), anim_color_from_hue(targetHue_),
                              t256);
        }

        case ColorMode::Static:
            break;  // handled above; keeps -Wswitch exhaustive
    }
    return raw & 0x00FFFFFFu;
}
