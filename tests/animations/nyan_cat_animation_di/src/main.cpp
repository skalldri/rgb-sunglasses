#include <zephyr/ztest.h>

#define private public
#include <animations/nyan_cat_animation.h>
#undef private

#include <animations/animation_renderer.h>

namespace {

static constexpr size_t kWidth  = 40;
static constexpr size_t kHeight = 12;

struct PixelState {
    uint8_t r = 0, g = 0, b = 0;
    bool isBlack() const { return r == 0 && g == 0 && b == 0; }
};

PixelState sPixels[kWidth][kHeight];

class CapturingRenderer : public AnimationRenderer {
   public:
    size_t displayWidth()  const override { return kWidth; }
    size_t displayHeight() const override { return kHeight; }
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
        if (x < kWidth && y < kHeight) sPixels[x][y] = {r, g, b};
    }
};

void resetPixels() {
    for (size_t x = 0; x < kWidth; x++)
        for (size_t y = 0; y < kHeight; y++)
            sPixels[x][y] = {};
}

bool anyPixelLit() {
    for (size_t x = 0; x < kWidth; x++)
        for (size_t y = 0; y < kHeight; y++)
            if (!sPixels[x][y].isBlack()) return true;
    return false;
}

bool anyPixelColored() {
    /* Returns true if any lit pixel has non-equal RGB channels (i.e. not just white/grey). */
    for (size_t x = 0; x < kWidth; x++)
        for (size_t y = 0; y < kHeight; y++)
            if (!sPixels[x][y].isBlack() &&
                (sPixels[x][y].r != sPixels[x][y].g || sPixels[x][y].g != sPixels[x][y].b))
                return true;
    return false;
}

}  // namespace

ZTEST_SUITE(nyan_cat_animation_di, NULL, NULL, NULL, NULL, NULL);

/* On native_sim there is no NAND filesystem; both open paths must fail → error state. */
ZTEST(nyan_cat_animation_di, test_setactive_no_file_enters_error_state)
{
    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_true(anim->inErrorState_, "Should be in error state when both file paths fail");
    anim->setActive(false);
}

/* Error state renders the rainbow "NO FILE" message — at least one pixel lit. */
ZTEST(nyan_cat_animation_di, test_error_state_tick_renders_pixels)
{
    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_true(anim->inErrorState_, "Precondition: must be in error state");

    CapturingRenderer renderer;
    resetPixels();
    anim->tick(renderer, 50);

    zassert_true(anyPixelLit(), "Error state should render 'NO FILE' with at least one lit pixel");
    anim->setActive(false);
}

/* The rainbow error message uses actual colors (not just white). */
ZTEST(nyan_cat_animation_di, test_error_state_renders_rainbow_colors)
{
    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);

    CapturingRenderer renderer;
    /* Advance far enough to show multiple characters (each has a different rainbow color). */
    for (int i = 0; i < 10; i++) {
        resetPixels();
        anim->tick(renderer, 50);
    }

    zassert_true(anyPixelColored(),
                 "Rainbow error should produce pixels with non-greyscale colors");
    anim->setActive(false);
}

/* setActive(false) must clear the error state. */
ZTEST(nyan_cat_animation_di, test_setactive_false_clears_error_state)
{
    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_true(anim->inErrorState_, "Precondition: must be in error state");

    anim->setActive(false);
    zassert_false(anim->inErrorState_, "setActive(false) must clear inErrorState_");
}
