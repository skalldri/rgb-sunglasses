#include <animations/null_animation.h>

void NullAnimation::init() {
    // Nothing
}

void NullAnimation::tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
    ARG_UNUSED(timeSinceLastTickMs);
    for (size_t x = 0; x < renderer.displayWidth(); x++) {
        for (size_t y = 0; y < renderer.displayHeight(); y++) {
            renderer.setPixel(x, y, 0, 0, 0);
        }
    }
}