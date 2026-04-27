#include <animations/zigzag_animation.h>

#include <zephyr/sys/__assert.h>

void ZigZagAnimation::setDependencies(const ZigZagAnimationDependencies &deps)
{
    deps_ = &deps;
}

void ZigZagAnimation::init()
{
    currentCycleTimeMs = 0;
    currentIndex = 0;
}

void ZigZagAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs)
{
    __ASSERT(deps_, "ZigZagAnimation::tick before setDependencies");

    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > deps_->stepTimeMs.get())
    {
        currentCycleTimeMs = 0;
        currentIndex++;
    }

    if (currentIndex >= (renderer.displayWidth() * renderer.displayHeight()))
    {
        currentIndex = 0;
    }

    for (size_t x = 0; x < renderer.displayWidth(); x++)
    {
        for (size_t y = 0; y < renderer.displayHeight(); y++)
        {
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
