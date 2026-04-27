#pragma once
#include <cstddef>
#include <cstdint>

class AnimationRenderer {
public:
    virtual ~AnimationRenderer() = default;
    virtual size_t displayWidth() const = 0;
    virtual size_t displayHeight() const = 0;
    virtual void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) = 0;
};
