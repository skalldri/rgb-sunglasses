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

    // Same kFastestStepTimeMs floor as Rainbow/ZigZag/Text (PR #378 review round 6):
    // drop_speed_ms is remotely writable with no range validation and persists, and
    // before #376 every value in 0..11 stepped one row per 11 ms tick — without the
    // floor, a persisted 1 ms drop speed would sweep the whole panel inside a single
    // 33 ms tick (spawn to bottom-exit, column never visibly lit) instead of the
    // ~90 rows/s every deployed firmware produced. The floor also guarantees the
    // carry loop below consumes >= kFastestStepTimeMs of budget per iteration.
    const uint32_t dropSpeedMs = std::max(kFastestStepTimeMs, deps_->dropSpeedMs.get());
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
            // Consume the tick's time budget with carry so the drop rate stays
            // wall-clock correct at any render tick rate — one tick can step the
            // head several rows when dropSpeedMs < the tick interval (issue #376).
            // Bounded: timeSinceLastTickMs is the NOMINAL configured interval
            // (pattern_controller passes kTargetRenderIntervalMs, never measured
            // elapsed time), so iterations <= dt/dropSpeed and there is no
            // post-stall catch-up spike; each iteration consumes >= 1 ms of
            // budget (dropSpeedMs is floored at 1 above).
            uint32_t budgetMs = timeSinceLastTickMs;
            bool stepped = false;
            while (columns_[x].active && budgetMs >= columns_[x].dropTimerMs) {
                budgetMs -= columns_[x].dropTimerMs;

                // Time to step the head down one row
                columns_[x].dropTimerMs = dropSpeedMs;
                columns_[x].headY++;
                stepped = true;

                if (columns_[x].headY >= height) {
                    // Drop has exited the bottom; deactivate this column
                    columns_[x].active = false;
                } else {
                    // Light the row, aged by the time already elapsed since this
                    // step within the tick (the remaining budget): the tick-level
                    // decay pass above ran BEFORE this loop, so without this a
                    // multi-row sweep would paint every crossed row at a flat 255
                    // and the trail would lose its falling gradient (PR #378
                    // review). The FINAL head position is re-lit at 255 below.
                    const uint32_t age = (budgetMs * 255) / fadeTimeMs;
                    brightness_[x][columns_[x].headY] =
                        (age < 255) ? (uint8_t)(255 - age) : 0;
                }
            }
            if (columns_[x].active) {
                columns_[x].dropTimerMs -= budgetMs;
                if (stepped) {
                    // The head itself stays a steady 255, matching both the
                    // spawn below and the pre-#376 per-step behavior — only the
                    // INTERMEDIATE rows a multi-step sweep crossed keep their
                    // in-tick age (PR #378 review: aging the head too made it
                    // shimmer at ~70-95% and pop to 255 on every new spawn).
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
