#include <animations/zigzag_animation.h>
#include <zephyr/sys/__assert.h>

void ZigZagAnimation::setDependencies(const ZigZagAnimationDependencies &deps) {
    deps_ = &deps;
}

void ZigZagAnimation::init() {
    currentCycleTimeMs = 0;
    currentIndex = 0;
}

void ZigZagAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    __ASSERT(deps_, "ZigZagAnimation::tick before setDependencies");

    currentCycleTimeMs += timeSinceLastTickMs;

    // A step time of 0 means "fastest" = the historical ~90 steps/s wall-clock
    // rate, independent of the render tick rate (kFastestStepTimeMs rationale).
    uint32_t stepTimeMs = deps_->stepTimeMs.get();
    if (stepTimeMs == 0) {
        stepTimeMs = kFastestStepTimeMs;
    }

    // Bound the accumulator against parameter-history abuse (PR #378 review;
    // rationale in rainbow_animation.cpp — a huge-then-small remotely written
    // step time would otherwise run accumulated/step iterations in one tick).
    if (currentCycleTimeMs > stepTimeMs + timeSinceLastTickMs) {
        currentCycleTimeMs = stepTimeMs + timeSinceLastTickMs;
    }

    // Carry the remainder instead of resetting to 0 so the step rate stays
    // wall-clock correct at any render tick rate, including step times shorter
    // than the tick interval (issue #376).
    while (currentCycleTimeMs > stepTimeMs) {
        currentCycleTimeMs -= stepTimeMs;
        currentIndex++;
    }

    const size_t totalIndices = renderer.displayWidth() * renderer.displayHeight();
    if (totalIndices == 0) {
        return;  // degenerate renderer: nothing to draw, and %= 0 would fault
    }
    // Modulo (not reset-to-0) so a multi-step tick that overshoots the last
    // pixel keeps its phase within the wrapped-around pass.
    currentIndex %= totalIndices;

    for (size_t x = 0; x < renderer.displayWidth(); x++) {
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            renderer.setPixel(x, y, 0, 0, 0);
        }
    }

    size_t y = currentIndex / renderer.displayWidth();
    size_t x = currentIndex % renderer.displayWidth();

    uint32_t color = deps_->color.get();
    uint8_t red = (color >> 16) & 0xFF;
    uint8_t green = (color >> 8) & 0xFF;
    uint8_t blue = (color >> 0) & 0xFF;
    renderer.setPixel(x, y, red, green, blue);
}
