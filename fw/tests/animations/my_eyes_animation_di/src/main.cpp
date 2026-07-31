#include <animations/animation_parameter_source.h>
#include <animations/animation_renderer.h>
#include <zephyr/ztest.h>

#define private public
#include <animations/my_eyes_animation.h>
#undef private

#include <cstring>

namespace {
class NullTestRenderer : public AnimationRenderer {
   public:
    size_t displayWidth() const override { return 40; }
    size_t displayHeight() const override { return 12; }
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
        ARG_UNUSED(x);
        ARG_UNUSED(y);
        ARG_UNUSED(r);
        ARG_UNUSED(g);
        ARG_UNUSED(b);
    }
};

class ConstUint32Source : public AnimationUint32ParameterSource {
   public:
    explicit ConstUint32Source(uint32_t value) : value_(value) {}

    uint32_t get() const override { return value_; }

   private:
    uint32_t value_;
};

class SequenceUpNextSource : public MyEyesAnimationUpNextSource {
   public:
    size_t consumeCurrentAndAdvance(size_t numSlots) override {
        lastNumSlots = numSlots;
        size_t value = sequence[index % 2];
        index++;
        return value;
    }

    size_t sequence[2] = {0, 1};
    size_t index = 0;
    size_t lastNumSlots = 0;
};

class FixedSlotSource : public MyEyesAnimationSlotSource {
   public:
    const char *getStringFromSlot(size_t slot) const override {
        if (slot == 0) {
            return "^^";
        }
        if (slot == 1) {
            return "@@";
        }

        return "00";
    }
};
}  // namespace

ZTEST_SUITE(my_eyes_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(my_eyes_animation_di_tests, test_init_uses_injected_slot_and_upnext_sources) {
    ConstUint32Source blinkSpeedMs(10);
    ConstUint32Source color(0xAABBCC);
    ConstUint32Source dwellTimeMs(10000);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;

    MyEyesAnimationDependencies deps(blinkSpeedMs, color, slotSource, upNextSource,
                                     dwellTimeMs);

    MyEyesAnimation *animation = MyEyesAnimation::getInstance();
    animation->setDependencies(deps);

    animation->init();
    zassert_true(strcmp(animation->currentEyes, "^^") == 0,
                 "Expected first injected eyes to be ^^");

    animation->init();
    zassert_true(strcmp(animation->currentEyes, "@@") == 0,
                 "Expected second injected eyes to be @@");
}

ZTEST(my_eyes_animation_di_tests, test_init_passes_slot_count_to_upnext_source) {
    ConstUint32Source blinkSpeedMs(10);
    ConstUint32Source color(0xAABBCC);
    ConstUint32Source dwellTimeMs(10000);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;

    MyEyesAnimationDependencies deps(blinkSpeedMs, color, slotSource, upNextSource,
                                     dwellTimeMs);

    MyEyesAnimation *animation = MyEyesAnimation::getInstance();
    animation->setDependencies(deps);

    animation->init();

    zassert_equal(upNextSource.lastNumSlots, 20,
                  "Expected up-next source to receive eye slot count");
}

ZTEST(my_eyes_animation_di_tests, test_tick_renders_pixels_at_both_eye_positions) {
    // Inject "^^" so both eyes have the same character
    ConstUint32Source blinkSpeedMs(10000);
    ConstUint32Source color(0xFFFFFF);
    ConstUint32Source dwellTimeMs(10000);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;
    MyEyesAnimationDependencies deps(blinkSpeedMs, color, slotSource, upNextSource,
                                     dwellTimeMs);

    MyEyesAnimation *animation = MyEyesAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();  // loads "^^"

    // Use a renderer wide enough for both eye positions (kLeftEyePos=5, kRightEyePos=28)
    static constexpr size_t kW = 40;
    static constexpr size_t kH = 12;
    static constexpr size_t kCharWidth = 7;  // atlasPixelWidthPerChar

    struct Pixel {
        uint8_t r, g, b;
    };
    Pixel pixels[kW][kH] = {};

    class EyeCapturingRenderer : public AnimationRenderer {
       public:
        Pixel (*pixels_)[kH];
        size_t displayWidth() const override { return kW; }
        size_t displayHeight() const override { return kH; }
        void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
            if (x < kW && y < kH) {
                pixels_[x][y] = {r, g, b};
            }
        }
    };

    EyeCapturingRenderer renderer;
    renderer.pixels_ = pixels;

    animation->tick(renderer, 1);

    // Count lit pixels in each eye region
    size_t leftEyeLitPixels = 0;
    size_t rightEyeLitPixels = 0;

    for (size_t x = MyEyesAnimation::kLeftEyePos; x < MyEyesAnimation::kLeftEyePos + kCharWidth;
         x++) {
        for (size_t y = 0; y < kH; y++) {
            if (pixels[x][y].r || pixels[x][y].g || pixels[x][y].b) {
                leftEyeLitPixels++;
            }
        }
    }

    for (size_t x = MyEyesAnimation::kRightEyePos; x < MyEyesAnimation::kRightEyePos + kCharWidth;
         x++) {
        for (size_t y = 0; y < kH; y++) {
            if (pixels[x][y].r || pixels[x][y].g || pixels[x][y].b) {
                rightEyeLitPixels++;
            }
        }
    }

    zassert_true(leftEyeLitPixels > 0, "Expected at least one lit pixel in left eye region");
    zassert_true(rightEyeLitPixels > 0, "Expected at least one lit pixel in right eye region");
}

// ---- Autonomous cycling (issue #260) ----------------------------------------------------
// Mirrors the text animation DI suite's dwell/grace coverage: tick() advances to the next
// slot once the configured dwell elapses, a remotely-written dwell of 0 is clamped to the
// 500 ms floor (the issue #188 notify-flood guard), and the shuffle good-switch-point
// signal fires only on the advancing tick.

namespace {
// One deps bundle per cycling test, built around a SequenceUpNextSource so advances are
// observable via its call count (index). init() itself consumes one advance.
struct CyclingFixture {
    explicit CyclingFixture(uint32_t dwellMs)
        : blinkSpeedMs(10), color(0xFFFFFF), dwellTimeMs(dwellMs),
          deps(blinkSpeedMs, color, slotSource, upNextSource, dwellTimeMs) {
        animation = MyEyesAnimation::getInstance();
        animation->setDependencies(deps);
        animation->init();
    }

    size_t advancesSinceInit() const { return upNextSource.index - 1; }

    ConstUint32Source blinkSpeedMs;
    ConstUint32Source color;
    ConstUint32Source dwellTimeMs;
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;
    MyEyesAnimationDependencies deps;
    MyEyesAnimation *animation;
};
}  // namespace

ZTEST(my_eyes_animation_di_tests, test_tick_advances_after_dwell_elapses) {
    NullTestRenderer renderer;
    CyclingFixture f(1000);

    // 999 ms in: still on the init()-loaded slot (sequence[0] == 0 -> "^^").
    f.animation->tick(renderer, 999);
    zassert_equal(f.advancesSinceInit(), 0, "Advanced before the dwell elapsed");
    zassert_true(strcmp(f.animation->currentEyes, "^^") == 0, "Eyes changed before the boundary");

    // Crossing 1000 ms advances exactly once and loads the next slot's string.
    f.animation->tick(renderer, 1);
    zassert_equal(f.advancesSinceInit(), 1, "Expected exactly one advance at the boundary");
    zassert_true(strcmp(f.animation->currentEyes, "@@") == 0,
                 "Expected the next slot's eyes after the advance");
}

ZTEST(my_eyes_animation_di_tests, test_zero_dwell_respects_min_floor) {
    NullTestRenderer renderer;
    CyclingFixture f(0);  // remote write of 0 must clamp to kMinEyeDwellMs (500)

    // 400 ms of 20 ms ticks: below the floor, no advance despite dwell == 0.
    for (int i = 0; i < 20; i++) {
        f.animation->tick(renderer, 20);
    }
    zassert_equal(f.advancesSinceInit(), 0, "Zero dwell must not advance before the 500 ms floor");

    // 200 ms more crosses the floor: exactly one advance, not one per tick.
    for (int i = 0; i < 10; i++) {
        f.animation->tick(renderer, 20);
    }
    zassert_equal(f.advancesSinceInit(), 1, "Expected exactly one advance after the floor");
}

ZTEST(my_eyes_animation_di_tests, test_good_switch_point_only_on_advance_tick) {
    NullTestRenderer renderer;
    CyclingFixture f(1000);

    f.animation->tick(renderer, 500);
    zassert_false(f.animation->isAtGoodSwitchPoint(), "Mid-dwell tick is not a switch point");

    f.animation->tick(renderer, 500);
    zassert_true(f.animation->isAtGoodSwitchPoint(),
                 "The advancing tick must report a good switch point");

    f.animation->tick(renderer, 10);
    zassert_false(f.animation->isAtGoodSwitchPoint(),
                  "The switch-point flag must clear on the next tick");
}

ZTEST(my_eyes_animation_di_tests, test_grace_request_tracks_remaining_dwell) {
    NullTestRenderer renderer;
    CyclingFixture f(1000);

    f.animation->tick(renderer, 300);
    zassert_equal(f.animation->goodSwitchPointGraceMs(), 700,
                  "Grace must report the dwell time still remaining");

    f.animation->tick(renderer, 300);
    zassert_equal(f.animation->goodSwitchPointGraceMs(), 400, "Grace must decrease with dwell");

    f.animation->tick(renderer, 400);
    zassert_equal(f.animation->goodSwitchPointGraceMs(), 0,
                  "Grace must be 0 on the boundary tick");
}

ZTEST(my_eyes_animation_di_tests, test_init_resets_dwell) {
    NullTestRenderer renderer;
    CyclingFixture f(1000);

    // Accumulate 900 of 1000 ms, then re-init (consumes one advance itself).
    f.animation->tick(renderer, 900);
    f.animation->init();
    size_t advancesAfterInit = f.upNextSource.index;

    // If init() didn't reset the accumulator, this 200 ms tick would advance.
    f.animation->tick(renderer, 200);
    zassert_equal(f.upNextSource.index, advancesAfterInit,
                  "init() must reset the dwell accumulator");
}
