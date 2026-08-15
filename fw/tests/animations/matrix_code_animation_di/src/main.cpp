#include <animations/animation_renderer.h>
#include <animations/matrix_code_animation.h>
#include <zephyr/ztest.h>

namespace {
class MutableUint32Source : public AnimationUint32ParameterSource {
   public:
    explicit MutableUint32Source(uint32_t value) : value_(value) {}

    uint32_t get() const override { return value_; }

    void set(uint32_t value) { value_ = value; }

   private:
    uint32_t value_;
};

// 1x8 display: a single column tall enough to observe multi-row head advances.
// Color is pure red (0xFF0000), so the captured red channel IS the brightness.
constexpr size_t kTestHeight = 8;
uint8_t sColumnRed[kTestHeight];

class ColumnCaptureRenderer : public AnimationRenderer {
   public:
    size_t displayWidth() const override { return 1; }
    size_t displayHeight() const override { return kTestHeight; }
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
        ARG_UNUSED(g);
        ARG_UNUSED(b);
        if (x == 0 && y < kTestHeight) {
            sColumnRed[y] = r;
        }
    }
};

void reset_capture() {
    for (size_t y = 0; y < kTestHeight; y++) {
        sColumnRed[y] = 0;
    }
}

// Highest row index with a lit pixel, or -1 if the column is dark.
int highest_lit_row() {
    for (int y = kTestHeight - 1; y >= 0; y--) {
        if (sColumnRed[y] > 0) {
            return y;
        }
    }
    return -1;
}

// Deterministically spawn the single column's drop: density=100 with a 1000 ms
// tick makes the spawn roll (rand % 100000 < density * dt) always succeed. The
// caller's density source is then zeroed so no later tick can respawn.
void spawn_drop(MatrixCodeAnimation *animation, ColumnCaptureRenderer &renderer,
                MutableUint32Source &density) {
    density.set(100);
    animation->tick(renderer, 1000);
    density.set(0);
}
}  // namespace

ZTEST_SUITE(matrix_code_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

// Issue #376: a drop speed shorter than the tick interval must step the head
// several rows in one tick (budget-with-carry), lighting every intermediate row —
// not be floored to one row per tick.
ZTEST(matrix_code_animation_di_tests, test_drop_advances_multiple_rows_in_one_tick) {
    MutableUint32Source dropSpeedMs(10);
    MutableUint32Source fadeTimeMs(600);
    MutableUint32Source density(0);
    MutableUint32Source color(0xFF0000);
    MatrixCodeAnimationDependencies deps(dropSpeedMs, fadeTimeMs, density, color);

    MatrixCodeAnimation *animation = MatrixCodeAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    ColumnCaptureRenderer renderer;
    spawn_drop(animation, renderer, density);  // head at row 0, timer = 10 ms

    reset_capture();
    // One 33 ms tick at a 10 ms drop speed: 33 → 23 → 13 → 3, exactly 3 rows.
    // Each swept row is aged by its remaining in-tick budget (PR #378 review:
    // a flat 255 sweep would lose the trail gradient): age = budget*255/600.
    animation->tick(renderer, 33);

    zassert_equal(sColumnRed[1], 246, "Expected row 1 aged by 23 ms (255 - 9)");
    zassert_equal(sColumnRed[2], 250, "Expected row 2 aged by 13 ms (255 - 5)");
    zassert_equal(sColumnRed[3], 254, "Expected row 3 aged by 3 ms (255 - 1)");
    zassert_equal(sColumnRed[4], 0, "Expected the head NOT to reach row 4 (3 steps only)");
    zassert_true(sColumnRed[1] < sColumnRed[2] && sColumnRed[2] < sColumnRed[3],
                 "Expected the falling gradient to brighten toward the head");
}

// Issue #376: head displacement must depend only on total elapsed time, not on how
// that time is partitioned into ticks (90 Hz and 30 Hz must render the same motion).
ZTEST(matrix_code_animation_di_tests, test_equal_displacement_across_tick_rates) {
    MutableUint32Source dropSpeedMs(200);
    MutableUint32Source fadeTimeMs(600);
    MutableUint32Source density(0);
    MutableUint32Source color(0xFF0000);
    MatrixCodeAnimationDependencies deps(dropSpeedMs, fadeTimeMs, density, color);

    MatrixCodeAnimation *animation = MatrixCodeAnimation::getInstance();
    animation->setDependencies(deps);
    ColumnCaptureRenderer renderer;

    // 990 ms as 90 ticks of 11 ms (the old ~90 Hz render rate)...
    animation->init();
    spawn_drop(animation, renderer, density);
    for (int i = 0; i < 90; i++) {
        reset_capture();
        animation->tick(renderer, 11);
    }
    const int headAt90Hz = highest_lit_row();

    // ...and as 30 ticks of 33 ms (the ~30 Hz render rate).
    animation->init();
    spawn_drop(animation, renderer, density);
    for (int i = 0; i < 30; i++) {
        reset_capture();
        animation->tick(renderer, 33);
    }
    const int headAt30Hz = highest_lit_row();

    zassert_equal(headAt90Hz, headAt30Hz, "Displacement must not depend on tick partitioning");
    // 990 ms at 200 ms/row crosses 200/400/600/800 → head on row 4 either way.
    zassert_equal(headAt90Hz, 4, "Expected the head on row 4 after 990 ms at 200 ms/row");
}

// The multi-step loop must deactivate cleanly when the head runs off the bottom
// mid-tick, without touching rows beyond the display.
ZTEST(matrix_code_animation_di_tests, test_drop_exits_bottom_within_one_tick) {
    MutableUint32Source dropSpeedMs(1);
    MutableUint32Source fadeTimeMs(600);
    MutableUint32Source density(0);
    MutableUint32Source color(0xFF0000);
    MatrixCodeAnimationDependencies deps(dropSpeedMs, fadeTimeMs, density, color);

    MatrixCodeAnimation *animation = MatrixCodeAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    ColumnCaptureRenderer renderer;
    spawn_drop(animation, renderer, density);

    reset_capture();
    // 33 ms at 1 ms/row: the head sweeps rows 1..7 (each aged by its remaining
    // in-tick budget, so the trail keeps its gradient), then exits and deactivates.
    animation->tick(renderer, 33);
    for (size_t y = 1; y < kTestHeight; y++) {
        zassert_true(sColumnRed[y] >= 240, "Expected row %zu lit by the sweeping head", y);
        if (y + 1 < kTestHeight) {
            zassert_true(sColumnRed[y] <= sColumnRed[y + 1],
                         "Expected brightness to rise toward the head at row %zu", y);
        }
    }
    const uint8_t bottomAfterSweep = sColumnRed[kTestHeight - 1];

    // The column is now inactive: the next tick only decays (33*255/600 = 14).
    reset_capture();
    animation->tick(renderer, 33);
    zassert_equal(sColumnRed[kTestHeight - 1], bottomAfterSweep - 14,
                  "Expected pure decay (no new head) after the drop exited the bottom");
}
