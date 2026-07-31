#include <animations/my_eyes_animation.h>
#include <fonts/FontAtlas.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>

#include <algorithm>
#include <cstring>

LOG_MODULE_REGISTER(my_eyes_animation, LOG_LEVEL_INF);

// Minimum time a slot's eyes stay on screen before we advance to the next slot,
// regardless of how small the remotely-writable dwell time is set. Same rationale as
// TextAnimation's kMinMessageDwellMs (issue #188 follow-up): each advance calls
// getUpNext(), which fires two GATT notifications (up next + now playing), so a dwell
// of 0 must not advance every render tick and flood the shared BT TX buffer pool.
// Caps advances at ~2/s, which the pool absorbs comfortably. Clamped here in tick()
// rather than rejecting the GATT write — a written 0 simply means "as fast as allowed".
static constexpr size_t kMinEyeDwellMs = 500;

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
    currentEyesDwellMs = 0;
    atGoodSwitchPoint_ = false;
    remainingDwellMs_ = 0;
    strncpy(currentEyes, getStringFromSlot(getUpNext()), kMaxEyeLen);
}

void MyEyesAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    __ASSERT(deps_, "MyEyesAnimation::tick before setDependencies");

    // The blink state machine below is still a stub; blinkSpeedMs stays unused.
    ARG_UNUSED(deps_->blinkSpeedMs);

    // Only the tick that advances to the next slot (below) is a good switch point.
    atGoodSwitchPoint_ = false;

    // Autonomous slot cycling (issue #260): advance to the next slot once the current
    // eyes have been displayed for the configured dwell time, mirroring how Text
    // advances at end-of-scroll. Advance-then-render so the new eyes draw on the
    // boundary frame.
    currentEyesDwellMs += timeSinceLastTickMs;
    const size_t dwellMs = std::max<size_t>(deps_->dwellTimeMs.get(), kMinEyeDwellMs);
    remainingDwellMs_ = (currentEyesDwellMs >= dwellMs)
                            ? 0u
                            : (uint32_t)(dwellMs - currentEyesDwellMs);
    if (currentEyesDwellMs >= dwellMs) {
        currentEyesDwellMs = 0;
        atGoodSwitchPoint_ = true;
        strncpy(currentEyes, getStringFromSlot(getUpNext()), kMaxEyeLen);
    }

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
