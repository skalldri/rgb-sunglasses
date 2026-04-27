#include <animations/my_eyes_animation.h>
#include <fonts/FontAtlas.h>
#include <cstring>

#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>

LOG_MODULE_REGISTER(my_eyes_animation, LOG_LEVEL_INF);

MyEyesAnimation::MyEyesAnimation() = default;

const char *MyEyesAnimation::getStringFromSlot(size_t slot)
{
    __ASSERT(deps_, "MyEyesAnimation::getStringFromSlot before setDependencies");

    return deps_->slotSource.getStringFromSlot(slot);
}

size_t MyEyesAnimation::getUpNext()
{
    __ASSERT(deps_, "MyEyesAnimation::getUpNext before setDependencies");

    return deps_->upNextSource.consumeCurrentAndAdvance(kNumStringSlots);
}

void MyEyesAnimation::setDependencies(const MyEyesAnimationDependencies &deps)
{
    deps_ = &deps;
}

void MyEyesAnimation::init()
{
    strncpy(currentEyes, getStringFromSlot(getUpNext()), kMaxEyeLen);
}

void MyEyesAnimation::tick(const LedConfig *config, const size_t timeSinceLastTickMs, const size_t bufferId)
{
    __ASSERT(deps_, "MyEyesAnimation::tick before setDependencies");

    ARG_UNUSED(deps_->blinkSpeedMs);

    // Turn off all LEDs
    for (size_t x = 0; x < config->displayWidth; x++)
    {
        for (size_t y = 0; y < config->displayHeight; y++)
        {
            pattern_controller_set_pixel_in_framebuffer(config, x, y, bufferId, 0, 0, 0);
        }
    }

    int32_t charWindowPos;

    // This function gets called repeatedly to render to the display
    auto lambda = [&](size_t x, size_t y, bool filled)
    {
        int32_t realX = x + charWindowPos;

        if (realX < 0 || realX >= (int32_t)config->displayWidth)
        {
            // Bail early if this pixel is not on the display
            return;
        }

        // If pixel is filled, fill with white
        if (filled)
        {
            uint32_t color = deps_->color.get();
            uint8_t red = (color >> 16) & 0xFF;
            uint8_t green = (color >> 8) & 0xFF;
            uint8_t blue = (color >> 0) & 0xFF;
            pattern_controller_set_pixel_in_framebuffer(config, realX, y, bufferId, red, green, blue);
        }
    };

    switch (currentEyeState)
    {
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
