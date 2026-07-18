#include <animations/pulse_animation.h>
#include <zephyr/sys/__assert.h>

void PulseAnimation::setDependencies(const PulseAnimationDependencies &deps) {
    deps_ = &deps;
}

void PulseAnimation::init() {
    currentCycleTimeMs = 0;
}

void PulseAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    __ASSERT(deps_, "PulseAnimation::tick before setDependencies");

    // Guard against a zero (or otherwise degenerate) period_ms making the modulo
    // below divide by zero; a 1ms period just breathes as fast as tick() allows.
    uint32_t periodMs = deps_->periodMs.get();
    if (periodMs == 0) {
        periodMs = 1;
    }

    currentCycleTimeMs += timeSinceLastTickMs;
    currentCycleTimeMs %= periodMs;

    // Triangle-wave breathing envelope: ramps 0 -> 1 across the first half of
    // the period, then back 1 -> 0 across the second half.
    float phase = (float)currentCycleTimeMs / (float)periodMs;
    float brightness = (phase < 0.5f) ? (phase * 2.0f) : (2.0f - (phase * 2.0f));

    uint32_t color = deps_->color.get();
    float red = (float)((color >> 16) & 0xFF) * brightness;
    float green = (float)((color >> 8) & 0xFF) * brightness;
    float blue = (float)((color >> 0) & 0xFF) * brightness;

    for (size_t x = 0; x < renderer.displayWidth(); x++) {
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            renderer.setPixel(x, y, red, green, blue);
        }
    }
}
