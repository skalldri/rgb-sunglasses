#include <animations/my_eyes_animation.h>
#include <fonts/FontAtlas.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>

#include <cstring>

LOG_MODULE_REGISTER(my_eyes_animation, LOG_LEVEL_INF);


MyEyesAnimation::MyEyesAnimation() = default;

const char *MyEyesAnimation::getStringFromSlot(size_t slot) {
    __ASSERT(deps_, "MyEyesAnimation::getStringFromSlot before setDependencies");

    return deps_->slotSource.getStringFromSlot(slot);
}

size_t MyEyesAnimation::getUpNext() {
    __ASSERT(deps_, "MyEyesAnimation::getUpNext before setDependencies");

    return deps_->upNextSource.consumeCurrentAndAdvance(kNumStringSlots);
}

void MyEyesAnimation::setDependencies(const MyEyesAnimationDependencies &deps) {
    deps_ = &deps;
}

void MyEyesAnimation::init() {
    // Discards any advance still pending from a boundary tick (getUpNext() below is
    // this activation's one consume — a stale pending flag would double-consume).
    dwellTracker_.reset();
    strncpy(currentEyes, getStringFromSlot(getUpNext()), kMaxEyeLen);
}

void MyEyesAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    __ASSERT(deps_, "MyEyesAnimation::tick before setDependencies");

    // The blink state machine below is still a stub; blinkSpeedMs stays unused.
    ARG_UNUSED(deps_->blinkSpeedMs);

    // Autonomous slot cycling (issue #260): advance to the next slot once the current
    // eyes have displayed for the configured dwell time, mirroring how Text advances at
    // end-of-scroll. The consume is deferred one tick past the reported boundary so a
    // shuffle switch taken at the boundary never eats a queued slot — full semantics on
    // SlotDwellTracker (animation_base.h).
    if (dwellTracker_.consumePendingAdvance()) {
        strncpy(currentEyes, getStringFromSlot(getUpNext()), kMaxEyeLen);
    }
    dwellTracker_.accumulate(timeSinceLastTickMs, deps_->dwellTimeMs.get());

    // Turn off all LEDs
    for (size_t x = 0; x < renderer.displayWidth(); x++) {
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            renderer.setPixel(x, y, 0, 0, 0);
        }
    }

    int32_t charWindowPos;

    // Resolve the color ONCE per tick, not per pixel: the source may be a
    // ColorModeSource (issue #259), whose get() is a stateful per-tick step that
    // can drain the audio message queue (RandomOnBeat). Calling it per lit pixel
    // put a kernel msgq drain in the inner render loop. Every other animation
    // already reads its color once per tick — match them.
    const uint32_t color = deps_->color.get();

    // This function gets called repeatedly to render to the display
    auto lambda = [&](size_t x, size_t y, bool filled) {
        int32_t realX = x + charWindowPos;

        if (realX < 0 || realX >= (int32_t)renderer.displayWidth()) {
            // Bail early if this pixel is not on the display
            return;
        }

        // If pixel is filled, fill with the resolved color
        if (filled) {
            uint8_t red = (color >> 16) & 0xFF;
            uint8_t green = (color >> 8) & 0xFF;
            uint8_t blue = (color >> 0) & 0xFF;
            renderer.setPixel(realX, y, red, green, blue);
        }
    };

    switch (currentEyeState) {
        case EyeState::Open:
            // Just draw our current character in each eye
            charWindowPos = kLeftEyePos;
            FontAtlas::getInstance()->PrintChar(currentEyes[0], lambda);

            charWindowPos = kRightEyePos;
            FontAtlas::getInstance()->PrintChar(currentEyes[1], lambda);
            break;

        case EyeState::OpenInBlinkCycle:
            break;

        case EyeState::BlinkClosing:
            break;

        case EyeState::BlinkOpening:
            break;

        case EyeState::Closed:
            break;
    }
}
