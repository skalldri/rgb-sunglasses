#include <animations/animation_renderer.h>
#include <animations/pulse_animation.h>
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

class MutableBoolSource : public AnimationBoolParameterSource {
   public:
    explicit MutableBoolSource(bool value) : value_(value) {}

    bool get() const override { return value_; }

    void set(bool value) { value_ = value; }

   private:
    bool value_;
};

// Fires a beat on demand. Mirrors the production latch: consumeBeat() reports at most
// one beat per call and clears itself.
class TestBeatSource : public AnimationBeatSource {
   public:
    bool consumeBeat() override {
        const bool beat = pending_;
        pending_ = false;
        return beat;
    }

    void fire() { pending_ = true; }

   private:
    bool pending_ = false;
};

struct PixelColor {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
};

PixelColor sPixelColor;

class CapturingTestRenderer : public AnimationRenderer {
   public:
    size_t displayWidth() const override { return 2; }
    size_t displayHeight() const override { return 1; }
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
        ARG_UNUSED(x);
        ARG_UNUSED(y);
        // Pulse writes the same color to every pixel every tick, so the last
        // write observed is representative of the whole frame.
        sPixelColor.red = r;
        sPixelColor.green = g;
        sPixelColor.blue = b;
    }
};

void reset_capture() {
    sPixelColor = {};
}
}  // namespace

// PulseAnimation is a singleton, so a beat source wired by one test would otherwise
// leak into the next.
void before_each(void *fixture) {
    ARG_UNUSED(fixture);
    PulseAnimation::getInstance()->setBeatSource(nullptr);
}

ZTEST_SUITE(pulse_animation_di_tests, NULL, NULL, before_each, NULL, NULL);

ZTEST(pulse_animation_di_tests, test_injected_color_and_period_at_cycle_start_is_dark) {
    MutableUint32Source color(0xFFFFFF);
    MutableUint32Source periodMs(1000);
    MutableBoolSource breathing(true);
    MutableBoolSource beatSync(false);
    PulseAnimationDependencies deps(color, periodMs, breathing, beatSync);

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;
    reset_capture();
    animation->tick(renderer, 0);

    zassert_equal(sPixelColor.red, 0, "Expected panel dark at the start of the breathing cycle");
    zassert_equal(sPixelColor.green, 0, "Expected panel dark at the start of the breathing cycle");
    zassert_equal(sPixelColor.blue, 0, "Expected panel dark at the start of the breathing cycle");
}

ZTEST(pulse_animation_di_tests, test_injected_period_reaches_full_brightness_at_half_cycle) {
    MutableUint32Source color(0x112233);
    MutableUint32Source periodMs(1000);
    MutableBoolSource breathing(true);
    MutableBoolSource beatSync(false);
    PulseAnimationDependencies deps(color, periodMs, breathing, beatSync);

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;
    reset_capture();
    animation->tick(renderer, 500);  // exactly half of period_ms -> peak of the triangle wave

    zassert_equal(sPixelColor.red, 0x11, "Expected full-brightness injected red component");
    zassert_equal(sPixelColor.green, 0x22, "Expected full-brightness injected green component");
    zassert_equal(sPixelColor.blue, 0x33, "Expected full-brightness injected blue component");
}

ZTEST(pulse_animation_di_tests, test_injected_period_changes_phase_at_fixed_elapsed_time) {
    MutableUint32Source color(0xFFFFFF);
    MutableUint32Source periodMs(1000);
    MutableBoolSource breathing(true);
    MutableBoolSource beatSync(false);
    PulseAnimationDependencies deps(color, periodMs, breathing, beatSync);

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;
    reset_capture();
    animation->tick(renderer, 100);  // 100/1000 = 10% into the cycle
    PixelColor slowPeriod = sPixelColor;

    periodMs.set(200);
    animation->init();

    reset_capture();
    animation->tick(renderer, 100);  // 100/200 = 50% into the cycle -> peak
    PixelColor fastPeriod = sPixelColor;

    zassert_true(fastPeriod.red > slowPeriod.red,
                "Expected a shorter injected period to reach a brighter point sooner for the "
                "same elapsed time");
}

ZTEST(pulse_animation_di_tests, test_period_wraps_cleanly_across_multiple_cycles) {
    MutableUint32Source color(0xFFFFFF);
    MutableUint32Source periodMs(1000);
    MutableBoolSource breathing(true);
    MutableBoolSource beatSync(false);
    PulseAnimationDependencies deps(color, periodMs, breathing, beatSync);

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;
    reset_capture();
    animation->tick(renderer, 2500);  // 2.5 cycles -> lands at the same phase as 500ms (peak)

    zassert_equal(sPixelColor.red, 255, "Expected the cycle to wrap and land back at peak");
    zassert_equal(sPixelColor.green, 255, "Expected the cycle to wrap and land back at peak");
    zassert_equal(sPixelColor.blue, 255, "Expected the cycle to wrap and land back at peak");
}

ZTEST(pulse_animation_di_tests, test_zero_period_does_not_crash) {
    MutableUint32Source color(0xFFFFFF);
    MutableUint32Source periodMs(0);
    MutableBoolSource breathing(true);
    MutableBoolSource beatSync(false);
    PulseAnimationDependencies deps(color, periodMs, breathing, beatSync);

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;
    reset_capture();

    // Must not divide-by-zero / crash; the animation clamps period_ms to at
    // least 1ms internally.
    animation->tick(renderer, 5);
}

ZTEST(pulse_animation_di_tests, test_breathing_off_holds_constant_full_brightness) {
    MutableUint32Source color(0x112233);
    MutableUint32Source periodMs(1000);
    MutableBoolSource breathing(false);
    MutableBoolSource beatSync(false);
    PulseAnimationDependencies deps(color, periodMs, breathing, beatSync);

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->init();

    CapturingTestRenderer renderer;

    // The flashlight case: full brightness at the phase that would be the darkest
    // point of the breathing envelope, and unchanged a quarter cycle later.
    reset_capture();
    animation->tick(renderer, 0);
    zassert_equal(sPixelColor.red, 0x11, "Expected full brightness with breathing off");
    zassert_equal(sPixelColor.green, 0x22, "Expected full brightness with breathing off");
    zassert_equal(sPixelColor.blue, 0x33, "Expected full brightness with breathing off");

    reset_capture();
    animation->tick(renderer, 250);
    zassert_equal(sPixelColor.red, 0x11, "Expected brightness not to vary with breathing off");
    zassert_equal(sPixelColor.green, 0x22, "Expected brightness not to vary with breathing off");
    zassert_equal(sPixelColor.blue, 0x33, "Expected brightness not to vary with breathing off");
}

ZTEST(pulse_animation_di_tests, test_beat_sync_snaps_to_full_brightness_on_a_beat) {
    MutableUint32Source color(0xFFFFFF);
    MutableUint32Source periodMs(1000);
    MutableBoolSource breathing(false);
    MutableBoolSource beatSync(true);
    PulseAnimationDependencies deps(color, periodMs, breathing, beatSync);
    TestBeatSource beats;

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->setBeatSource(&beats);
    animation->init();

    CapturingTestRenderer renderer;

    // Dark until the first beat arrives.
    reset_capture();
    animation->tick(renderer, 10);
    zassert_equal(sPixelColor.red, 0, "Expected the panel dark before the first beat");

    beats.fire();
    reset_capture();
    animation->tick(renderer, 10);
    zassert_equal(sPixelColor.red, 255, "Expected a beat to snap to full brightness");
    zassert_equal(sPixelColor.green, 255, "Expected a beat to snap to full brightness");
    zassert_equal(sPixelColor.blue, 255, "Expected a beat to snap to full brightness");
}

ZTEST(pulse_animation_di_tests, test_beat_sync_ramps_down_over_half_a_period) {
    MutableUint32Source color(0xFFFFFF);
    MutableUint32Source periodMs(1000);
    MutableBoolSource breathing(false);
    MutableBoolSource beatSync(true);
    PulseAnimationDependencies deps(color, periodMs, breathing, beatSync);
    TestBeatSource beats;

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->setBeatSource(&beats);
    animation->init();

    CapturingTestRenderer renderer;

    // First tick performs the beat-sync entry re-arm (which discards any stale latch),
    // so the beat under test has to be fired after it.
    animation->tick(renderer, 10);
    beats.fire();
    animation->tick(renderer, 10);

    // Half the 500ms decay elapsed -> roughly half brightness.
    reset_capture();
    animation->tick(renderer, 250);
    zassert_within(sPixelColor.red, 127, 2, "Expected ~half brightness a quarter period after a beat");

    // The remainder of the decay -> fully dark, and it stays there without a new beat.
    reset_capture();
    animation->tick(renderer, 250);
    zassert_equal(sPixelColor.red, 0, "Expected the ramp to reach dark half a period after a beat");

    reset_capture();
    animation->tick(renderer, 250);
    zassert_equal(sPixelColor.red, 0, "Expected the panel to stay dark until the next beat");
}

ZTEST(pulse_animation_di_tests, test_beat_sync_takes_precedence_over_breathing) {
    MutableUint32Source color(0xFFFFFF);
    MutableUint32Source periodMs(1000);
    // Only reachable from settings written by some other path: the BLE adapter clears
    // one when the other is written. tick() must still pick a single envelope.
    MutableBoolSource breathing(true);
    MutableBoolSource beatSync(true);
    PulseAnimationDependencies deps(color, periodMs, breathing, beatSync);
    TestBeatSource beats;

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->setBeatSource(&beats);
    animation->init();

    CapturingTestRenderer renderer;

    // Half a period in, the breathing envelope would be at its peak. With no beat
    // delivered, the beat envelope is at zero instead.
    reset_capture();
    animation->tick(renderer, 500);
    zassert_equal(sPixelColor.red, 0, "Expected beat sync to override the breathing envelope");
}

ZTEST(pulse_animation_di_tests, test_beat_sync_without_a_beat_source_stays_lit) {
    MutableUint32Source color(0x112233);
    MutableUint32Source periodMs(1000);
    MutableBoolSource breathing(false);
    MutableBoolSource beatSync(true);
    PulseAnimationDependencies deps(color, periodMs, breathing, beatSync);

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->setBeatSource(nullptr);  // audio-less build
    animation->init();

    CapturingTestRenderer renderer;
    reset_capture();
    animation->tick(renderer, 500);

    zassert_equal(sPixelColor.red, 0x11, "Expected beat sync without audio to fall back to solid");
    zassert_equal(sPixelColor.green, 0x22, "Expected beat sync without audio to fall back to solid");
    zassert_equal(sPixelColor.blue, 0x33, "Expected beat sync without audio to fall back to solid");
}

ZTEST(pulse_animation_di_tests, test_beat_sync_zero_period_does_not_crash) {
    MutableUint32Source color(0xFFFFFF);
    // period_ms 1 makes the half-period decay round to zero; the animation clamps it
    // to 1ms rather than dividing by zero (the beat-mode twin of
    // test_zero_period_does_not_crash).
    MutableUint32Source periodMs(1);
    MutableBoolSource breathing(false);
    MutableBoolSource beatSync(true);
    PulseAnimationDependencies deps(color, periodMs, breathing, beatSync);
    TestBeatSource beats;

    PulseAnimation *animation = PulseAnimation::getInstance();
    animation->setDependencies(deps);
    animation->setBeatSource(&beats);
    animation->init();

    CapturingTestRenderer renderer;
    animation->tick(renderer, 1);  // entry re-arm
    beats.fire();
    animation->tick(renderer, 1);

    reset_capture();
    animation->tick(renderer, 1);
    zassert_equal(sPixelColor.red, 0, "Expected a 1ms decay to be fully spent after 1ms");
}

/* Fixture for the cases below: owns the four sources so a new dependency field costs
 * one edit here rather than one per test (the two added for #148 cost five). */
namespace {
struct PulseFixture {
    MutableUint32Source color{0xFFFFFF};
    MutableUint32Source periodMs{1000};
    MutableBoolSource breathing{false};
    MutableBoolSource beatSync{false};
    TestBeatSource beats;
    PulseAnimationDependencies deps{color, periodMs, breathing, beatSync};

    PulseAnimation *start(bool withBeatSource = true) {
        PulseAnimation *animation = PulseAnimation::getInstance();
        animation->setDependencies(deps);
        animation->setBeatSource(withBeatSource ? &beats : nullptr);
        animation->init();
        return animation;
    }
};
}  // namespace

/* A beat latched while Pulse was inactive (or not in beat-sync mode) must not light the
 * panel on entry: nothing drains this consumer's slot while it is unused, so the latch
 * survives and the first consumeBeat() would otherwise flash full brightness in silence. */
ZTEST(pulse_animation_di_tests, test_stale_beat_does_not_flash_on_activation) {
    PulseFixture f;
    f.beatSync.set(true);

    // Beat arrives before this activation — e.g. while the Beat animation was running.
    f.beats.fire();

    PulseAnimation *animation = f.start();
    CapturingTestRenderer renderer;
    reset_capture();
    animation->tick(renderer, 10);

    zassert_equal(sPixelColor.red, 0, "Expected a pre-activation beat to be discarded");

    // A beat delivered after activation still works.
    f.beats.fire();
    reset_capture();
    animation->tick(renderer, 10);
    zassert_equal(sPixelColor.red, 255, "Expected a post-activation beat to light the panel");
}

/* Toggling Beat Sync is a BLE write, not an animation switch, so init() never runs.
 * Without an entry re-arm the envelope stays frozen at its last value and re-enabling
 * beat sync in a silent room lights the panel and fades it over half a period. */
ZTEST(pulse_animation_di_tests, test_beat_sync_re_enable_starts_dark) {
    PulseFixture f;
    f.beatSync.set(true);

    PulseAnimation *animation = f.start();
    CapturingTestRenderer renderer;

    animation->tick(renderer, 10);  // entry re-arm

    // Drive the envelope high, then leave beat sync without letting it decay.
    f.beats.fire();
    reset_capture();
    animation->tick(renderer, 10);
    zassert_equal(sPixelColor.red, 255, "Expected the envelope high before the toggle");

    f.beatSync.set(false);
    f.breathing.set(true);
    animation->tick(renderer, 10);

    // Back to beat sync, silent room: must start dark, not resume the frozen value.
    f.breathing.set(false);
    f.beatSync.set(true);
    reset_capture();
    animation->tick(renderer, 10);
    zassert_equal(sPixelColor.red, 0, "Expected re-enabling Beat Sync to start dark");
}

/* Beat sync wins over breathing even with no beat source bound — the audio-less
 * fallback is constant full brightness, not a fall-through to breathing. Pins the
 * two promises in pulse_animation.h against being folded back together. */
ZTEST(pulse_animation_di_tests, test_both_flags_without_beat_source_holds_full_brightness) {
    PulseFixture f;
    f.color.set(0x112233);
    f.breathing.set(true);
    f.beatSync.set(true);

    PulseAnimation *animation = f.start(/* withBeatSource = */ false);
    CapturingTestRenderer renderer;

    // Half a period in, breathing would be at its peak and a beat envelope at zero;
    // full brightness distinguishes the fallback from both.
    reset_capture();
    animation->tick(renderer, 500);
    zassert_equal(sPixelColor.red, 0x11, "Expected the no-beat-source fallback to hold lit");
    zassert_equal(sPixelColor.green, 0x22, "Expected the no-beat-source fallback to hold lit");
    zassert_equal(sPixelColor.blue, 0x33, "Expected the no-beat-source fallback to hold lit");

    // And it stays there rather than tracking the breathing phase.
    reset_capture();
    animation->tick(renderer, 250);
    zassert_equal(sPixelColor.red, 0x11, "Expected brightness not to follow the breathing phase");
}
