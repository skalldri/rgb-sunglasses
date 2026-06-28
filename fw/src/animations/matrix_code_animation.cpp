#include <animations/matrix_code_animation.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/__assert.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

void MatrixCodeAnimation::setDependencies(const MatrixCodeAnimationDependencies &deps) {
    deps_ = &deps;
}

void MatrixCodeAnimation::init() {
    for (size_t x = 0; x < kMatrixMaxCols; x++) {
        columns_[x] = {false, 0, 0};
        for (size_t y = 0; y < kMatrixMaxRows; y++) {
            brightness_[x][y] = 0;
        }
    }
}

void MatrixCodeAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    __ASSERT(deps_, "MatrixCodeAnimation::tick before setDependencies");

    const uint32_t dropSpeedMs = std::max(1u, deps_->dropSpeedMs.get());
    const uint32_t fadeTimeMs = std::max(1u, deps_->fadeTimeMs.get());
    const uint32_t density = std::min(100u, deps_->density.get());
    const uint32_t color = deps_->color.get();

    const uint8_t colorR = (color >> 16) & 0xFF;
    const uint8_t colorG = (color >> 8) & 0xFF;
    const uint8_t colorB = color & 0xFF;

    const size_t width = renderer.displayWidth();
    const size_t height = renderer.displayHeight();

    __ASSERT(width <= kMatrixMaxCols && height <= kMatrixMaxRows,
             "Display (%zu x %zu) exceeds MatrixCodeAnimation buffer (%zu x %zu)",
             width, height, kMatrixMaxCols, kMatrixMaxRows);

    // Decay all pixel brightnesses
    const uint32_t decay = (timeSinceLastTickMs * 255) / fadeTimeMs;
    for (size_t x = 0; x < width; x++) {
        for (size_t y = 0; y < height; y++) {
            brightness_[x][y] =
                (brightness_[x][y] > decay) ? (uint8_t)(brightness_[x][y] - decay) : 0;
        }
    }

    // Advance active drop heads and spawn new drops on inactive columns
    for (size_t x = 0; x < width; x++) {
        if (columns_[x].active) {
            if (columns_[x].dropTimerMs > timeSinceLastTickMs) {
                columns_[x].dropTimerMs -= timeSinceLastTickMs;
            } else {
                // Time to step the head down one row
                columns_[x].dropTimerMs = dropSpeedMs;
                columns_[x].headY++;

                if (columns_[x].headY >= height) {
                    // Drop has exited the bottom; deactivate this column
                    columns_[x].active = false;
                } else {
                    // Light up the new head position at full brightness
                    brightness_[x][columns_[x].headY] = 255;
                }
            }
        } else {
            // density (0-100) = % chance per second; scale to per-tick probability so
            // the effective spawn rate is tick-rate-independent.
            if (density > 0 && (sys_rand32_get() % 100000) < (density * timeSinceLastTickMs)) {
                columns_[x].active = true;
                columns_[x].headY = 0;
                columns_[x].dropTimerMs = dropSpeedMs;
                brightness_[x][0] = 255;
            }
        }
    }

    // Render pixels
    for (size_t x = 0; x < width; x++) {
        for (size_t y = 0; y < height; y++) {
            const uint8_t b = brightness_[x][y];
            renderer.setPixel(x, y, (colorR * b) / 255, (colorG * b) / 255,
                              (colorB * b) / 255);
        }
    }
}
