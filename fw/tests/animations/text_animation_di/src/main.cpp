#include <animations/animation_parameter_source.h>
#include <animations/animation_renderer.h>
#include <fonts/FontAtlas.h>
#include <zephyr/ztest.h>

#define private public
#include <animations/text_animation.h>
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

class SequenceUpNextSource : public TextAnimationUpNextSource {
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

class FixedSlotSource : public TextAnimationSlotSource {
   public:
    const char *getStringFromSlot(size_t slot) const override {
        if (slot == 0) {
            return "HELLO";
        }
        if (slot == 1) {
            return "WORLD";
        }

        return "UNKNOWN";
    }
};

class EmptySlotSource : public TextAnimationSlotSource {
   public:
    const char *getStringFromSlot(size_t slot) const override {
        ARG_UNUSED(slot);
        return "";
    }
};
}  // namespace

ZTEST_SUITE(text_animation_di_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(text_animation_di_tests, test_init_uses_injected_slot_and_upnext_sources) {
    ConstUint32Source stepTimeMs(10);
    ConstUint32Source color(0xAABBCC);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;

    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);

    animation->init();
    zassert_true(strcmp(animation->currentMessage, "HELLO") == 0,
                 "Expected first injected message to be HELLO");

    animation->init();
    zassert_true(strcmp(animation->currentMessage, "WORLD") == 0,
                 "Expected second injected message to be WORLD");
}

ZTEST(text_animation_di_tests, test_init_passes_slot_count_to_upnext_source) {
    ConstUint32Source stepTimeMs(10);
    ConstUint32Source color(0xAABBCC);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;

    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);

    animation->init();

    zassert_equal(upNextSource.lastNumSlots, 20,
                  "Expected up-next source to receive text slot count");
}

ZTEST(text_animation_di_tests, test_tick_accumulates_cycle_time) {
    ConstUint32Source stepTimeMs(1000);
    ConstUint32Source color(0xFFFFFF);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;
    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    NullTestRenderer renderer;
    animation->tick(renderer, 50);

    zassert_equal(animation->currentCycleTimeMs, 50,
                  "Expected cycle time to accumulate the tick delta");
}

ZTEST(text_animation_di_tests, test_tick_advances_offset_when_step_time_elapses) {
    ConstUint32Source stepTimeMs(10);
    ConstUint32Source color(0xFFFFFF);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;
    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    NullTestRenderer renderer;
    animation->tick(renderer, 15);  // 15 > stepTimeMs(10), should advance offset

    zassert_equal(animation->currentTextOffset, -1,
                  "Expected offset to decrement once when step time elapses");
}

ZTEST(text_animation_di_tests, test_tick_does_not_advance_offset_before_step_time) {
    ConstUint32Source stepTimeMs(1000);
    ConstUint32Source color(0xFFFFFF);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;
    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    NullTestRenderer renderer;
    animation->tick(renderer, 1);  // 1 < stepTimeMs(1000), offset unchanged

    zassert_equal(animation->currentTextOffset, 0,
                  "Expected offset unchanged when step time has not elapsed");
}

// Issue #376: a step time shorter than the tick interval must take several steps in
// one tick (carry-remainder accumulator), not be floored to one step per tick.
ZTEST(text_animation_di_tests, test_step_time_below_tick_advances_multiple_steps) {
    ConstUint32Source stepTimeMs(10);
    ConstUint32Source color(0xFFFFFF);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;
    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    NullTestRenderer renderer;
    animation->tick(renderer, 35);  // floor((35-1)/10) = 3 steps

    zassert_equal(animation->currentTextOffset, -3,
                  "Expected 3 pixel steps from one 35 ms tick at a 10 ms step time");
}

// Issue #376: total displacement must depend only on total elapsed time, not on how
// that time is partitioned into ticks (90 Hz and 30 Hz must render the same motion).
ZTEST(text_animation_di_tests, test_equal_displacement_across_tick_rates) {
    ConstUint32Source stepTimeMs(45);  // divides neither 11 nor 33 evenly
    ConstUint32Source color(0xFFFFFF);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;
    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);
    NullTestRenderer renderer;

    // 990 ms as 90 ticks of 11 ms (the old ~90 Hz render rate)...
    animation->init();
    for (int i = 0; i < 90; i++) {
        animation->tick(renderer, 11);
    }
    const int32_t offsetAt90Hz = animation->currentTextOffset;

    // ...and as 30 ticks of 33 ms (the ~30 Hz render rate).
    animation->init();
    for (int i = 0; i < 30; i++) {
        animation->tick(renderer, 33);
    }
    const int32_t offsetAt30Hz = animation->currentTextOffset;

    zassert_equal(offsetAt90Hz, offsetAt30Hz,
                  "Displacement must not depend on tick partitioning");
    zassert_equal(offsetAt90Hz, -21, "Expected exactly 21 steps in 990 ms");
}

// Regression (issue #188 follow-up): an empty slot satisfies "finished scrolling"
// (firstChar >= currentMessageLen == 0) on every tick, so without a minimum-dwell floor
// it advanced to the next slot - and fired GATT notifications via getUpNext() - at the
// full render rate, exhausting the BT TX buffer pool. The floor must bound advancement.
ZTEST(text_animation_di_tests, test_empty_slot_does_not_advance_every_tick) {
    ConstUint32Source stepTimeMs(10);
    ConstUint32Source color(0xFFFFFF);
    EmptySlotSource slotSource;
    SequenceUpNextSource upNextSource;
    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();  // one initial consume

    const size_t indexAfterInit = upNextSource.index;

    NullTestRenderer renderer;
    // 20 ticks * 20ms = 400ms, still under the 500ms dwell floor: an empty message must
    // NOT keep advancing (and notifying) on every tick.
    for (int i = 0; i < 20; i++) {
        animation->tick(renderer, 20);
    }
    zassert_equal(upNextSource.index, indexAfterInit,
                  "Empty slot must not advance before the dwell floor elapses");

    // Crossing the floor (400 + 200 = 600ms >= 500ms) reports the boundary; the consume
    // is deferred to the next tick (so a shuffle switch at the boundary can't eat a
    // queued slot), which advances exactly once.
    animation->tick(renderer, 200);
    zassert_equal(upNextSource.index, indexAfterInit,
                  "The boundary tick must not consume — the advance is deferred one tick");
    animation->tick(renderer, 20);
    zassert_equal(upNextSource.index, indexAfterInit + 1,
                  "Empty slot should advance exactly once after the dwell floor elapses");
}

ZTEST(text_animation_di_tests, test_good_switch_point_only_at_end_of_scroll) {
    ConstUint32Source stepTimeMs(1);  // scroll 1 px per tick (dt below always exceeds this)
    ConstUint32Source color(0xFFFFFF);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;
    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();
    zassert_false(animation->isAtGoodSwitchPoint(), "must not be a switch point after init");

    const size_t indexAfterInit = upNextSource.index;
    NullTestRenderer renderer;

    // Scroll until the message finishes; every tick before the finishing one must NOT
    // be a good switch point.
    bool sawGood = false;
    for (int i = 0; i < 5000; i++) {
        animation->tick(renderer, 20);
        if (animation->isAtGoodSwitchPoint()) {
            sawGood = true;
            break;
        }
        zassert_equal(upNextSource.index, indexAfterInit,
                      "advanced to the next message without signalling a switch point");
    }
    zassert_true(sawGood, "end of scroll never became a good switch point");
    // The boundary tick must NOT consume the queued slot — the advance is deferred one
    // tick so a shuffle switch taken at the boundary leaves the queue untouched.
    zassert_equal(upNextSource.index, indexAfterInit,
                  "the boundary tick must not consume the queued slot");

    // The next tick consumes the advance and starts the new message: the pulse clears.
    animation->tick(renderer, 20);
    zassert_equal(upNextSource.index, indexAfterInit + 1,
                  "the tick after the boundary must consume exactly one advance");
    zassert_false(animation->isAtGoodSwitchPoint(),
                  "switch point must be a one-tick pulse, not a latched state");
}

// ---------------------------------------------------------------------------
// goodSwitchPointGraceMs() — how long shuffle should wait for the end-of-scroll
// signal. Without it, shuffle hard-cuts any message longer than its dwell target
// plus CONFIG_APP_SHUFFLE_GRACE_S (30 s); a 255-char message scrolls for ~76 s.
// ---------------------------------------------------------------------------

namespace {

// Pixels the message must still scroll when currentTextOffset is 0 (i.e. right after
// init): the whole rendered string, plus the display it has to cross, plus the one-char
// edge buffer — mirroring text_animation.cpp's own completion test.
size_t fullScrollPixels(const char *msg, size_t displayWidth) {
    return strlen(msg) * FontAtlas::atlasPixelWidthPerChar + displayWidth +
           FontAtlas::atlasPixelWidthPerChar;
}

}  // namespace

ZTEST(text_animation_di_tests, test_grace_request_tracks_remaining_scroll) {
    ConstUint32Source stepTimeMs(10);
    ConstUint32Source color(0xFFFFFF);
    FixedSlotSource slotSource;  // slot 0 -> "HELLO"
    SequenceUpNextSource upNextSource;
    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    NullTestRenderer renderer;
    animation->tick(renderer, 1);  // dt below the step time: no pixel moves yet

    const size_t expected = fullScrollPixels("HELLO", renderer.displayWidth()) * 10u;
    zassert_equal(animation->goodSwitchPointGraceMs(), (uint32_t)expected,
                  "must ask for the whole remaining scroll (%zu ms)", expected);

    // Scrolling shrinks the request; it must never grow while one message is playing.
    uint32_t previous = animation->goodSwitchPointGraceMs();
    for (int i = 0; i < 20; i++) {
        animation->tick(renderer, 10);
        const uint32_t now = animation->goodSwitchPointGraceMs();
        zassert_true(now <= previous, "the request grew mid-message (%u -> %u)", previous, now);
        previous = now;
    }
    zassert_true(previous < expected, "the request never shrank as the message scrolled");
}

ZTEST(text_animation_di_tests, test_grace_request_zero_at_end_of_scroll) {
    ConstUint32Source stepTimeMs(1);  // scroll 1 px per tick (dt below always exceeds this)
    ConstUint32Source color(0xFFFFFF);
    FixedSlotSource slotSource;
    SequenceUpNextSource upNextSource;
    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    NullTestRenderer renderer;
    bool sawGood = false;
    for (int i = 0; i < 5000; i++) {
        animation->tick(renderer, 20);
        if (animation->isAtGoodSwitchPoint()) {
            sawGood = true;
            break;
        }
    }
    zassert_true(sawGood, "end of scroll never became a good switch point");
    zassert_equal(animation->goodSwitchPointGraceMs(), 0u,
                  "at the boundary itself there is nothing left to wait for");
}

ZTEST(text_animation_di_tests, test_grace_request_floors_at_min_dwell) {
    // An empty slot satisfies the "finished scrolling" test on every tick, so the only
    // thing shuffle would ever wait for is the kMinSlotDwellMs floor — not a whole
    // message's worth of scroll.
    ConstUint32Source stepTimeMs(10);
    ConstUint32Source color(0xFFFFFF);
    EmptySlotSource slotSource;
    SequenceUpNextSource upNextSource;
    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    NullTestRenderer renderer;
    animation->tick(renderer, 100);  // dwell 100 of the 500 ms floor
    zassert_equal(animation->goodSwitchPointGraceMs(), 400u,
                  "must ask only for the remaining dwell floor");

    animation->tick(renderer, 300);  // dwell 400
    zassert_equal(animation->goodSwitchPointGraceMs(), 100u, "floor must count down");

    animation->tick(renderer, 200);  // crosses the floor -> boundary (consume is next tick)
    zassert_equal(animation->goodSwitchPointGraceMs(), 0u,
                  "past the floor there is nothing left to wait for");
}

ZTEST(text_animation_di_tests, test_grace_request_zero_step_time) {
    // A 0 step time means "fastest" = a 1 ms step (wall-clock defined at any tick
    // rate, PR #378 review), so the per-pixel cost is 1 ms, not 0 — the request
    // must not collapse to nothing.
    ConstUint32Source stepTimeMs(0);
    ConstUint32Source color(0xFFFFFF);
    FixedSlotSource slotSource;  // slot 0 -> "HELLO"
    SequenceUpNextSource upNextSource;
    TextAnimationDependencies deps(stepTimeMs, color, slotSource, upNextSource);

    TextAnimation *animation = TextAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    NullTestRenderer renderer;
    const size_t full = fullScrollPixels("HELLO", renderer.displayWidth());

    // The request is a snapshot taken at the top of tick(), before that frame's
    // pixel steps — so the first tick still reports the full scroll, at 1 ms/px.
    animation->tick(renderer, 20);
    zassert_equal(animation->goodSwitchPointGraceMs(), (uint32_t)(full * 1u),
                  "a 0 step time must price a pixel at 1 ms, not 0 ms");

    // The first tick's 20 ms stepped floor((20-1)/1) = 19 pixels; the second
    // snapshot is exactly 19 ms less.
    animation->tick(renderer, 20);
    zassert_equal(animation->goodSwitchPointGraceMs(), (uint32_t)((full - 19u) * 1u),
                  "each scrolled pixel must retire one step time's worth of the request");
}
