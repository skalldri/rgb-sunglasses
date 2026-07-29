#include <animations/color_mode_source.h>
#include <zephyr/ztest.h>

#include <cstdint>

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

namespace {

struct FakeRawSource : public AnimationUint32ParameterSource {
    uint32_t value = 0;
    uint32_t get() const override { return value; }
};

// Scripted RNG: RandomFn is a plain function pointer, so the script is static
// (same pattern as the shuffle_controller suite). Exhausted -> returns 0.
uint32_t sRngSeq[32];
size_t sRngLen;
size_t sRngIdx;

uint32_t scripted_rng() {
    return (sRngIdx < sRngLen) ? sRngSeq[sRngIdx++] : 0u;
}

void rng_script(std::initializer_list<uint32_t> vals) {
    sRngLen = 0;
    sRngIdx = 0;
    for (uint32_t v : vals) {
        if (sRngLen < 32) {
            sRngSeq[sRngLen++] = v;
        }
    }
}

// Fake clock, advanced explicitly by each test.
int64_t sNowMs;

int64_t fake_now() {
    return sNowMs;
}

struct FakeBeatSource : public AnimationBeatSource {
    bool pending = false;
    bool consumeBeat() override {
        const bool b = pending;
        pending = false;
        return b;
    }
};

// The hue a reset rolls starting from internal hue state 0 with rng value r:
// rollHueFrom(0) = (0 + 256 + r % 1024) % 1536.
constexpr uint16_t roll_from(uint16_t base, uint32_t r) {
    return static_cast<uint16_t>((base + 256u + (r % 1024u)) % 1536u);
}

constexpr uint32_t mode_value(uint8_t mode, uint8_t speed) {
    return (static_cast<uint32_t>(mode) << 24) | (static_cast<uint32_t>(speed) << 16);
}

void suite_before(void *) {
    rng_script({});
    sNowMs = 0;
    // Undo any default-beat-source wiring a previous case left behind.
    ColorModeSource::setDefaultBeatSource(nullptr);
}

}  // namespace

ZTEST_SUITE(color_mode_source, NULL, NULL, suite_before, NULL, NULL);

// ---------------------------------------------------------------------------
// Static passthrough + unknown-mode fallback
// ---------------------------------------------------------------------------

ZTEST(color_mode_source, test_static_passthrough) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);

    raw.value = 0x00123456;
    zassert_equal(src.get(), 0x00123456u);

    // Mode byte always stripped from the output, whatever it is.
    raw.value = 0x00FFFFFF;
    zassert_equal(src.get(), 0x00FFFFFFu);
}

ZTEST(color_mode_source, test_unknown_modes_render_static) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);

    // 0xFF: the persisted pre-feature default (0xFFFFFFFF) on real devices.
    raw.value = 0xFFFFFFFF;
    zassert_equal(src.get(), 0x00FFFFFFu);

    raw.value = 0x05123456;  // first value past the last defined mode
    zassert_equal(src.get(), 0x00123456u);

    raw.value = 0x80ABCDEF;
    zassert_equal(src.get(), 0x00ABCDEFu);
}

// ---------------------------------------------------------------------------
// Hue wheel
// ---------------------------------------------------------------------------

ZTEST(color_mode_source, test_hue_wheel_corners) {
    zassert_equal(anim_color_from_hue(0), 0xFF0000u);     // red
    zassert_equal(anim_color_from_hue(256), 0xFFFF00u);   // yellow
    zassert_equal(anim_color_from_hue(512), 0x00FF00u);   // green
    zassert_equal(anim_color_from_hue(768), 0x00FFFFu);   // cyan
    zassert_equal(anim_color_from_hue(1024), 0x0000FFu);  // blue
    zassert_equal(anim_color_from_hue(1280), 0xFF00FFu);  // magenta
}

ZTEST(color_mode_source, test_hue_wheel_always_vivid) {
    // Every hue must have at least one channel at 255, or the global-brightness
    // scaling downstream renders it invisibly dim (fw/CLAUDE.md).
    for (uint16_t hue = 0; hue < 1536; hue++) {
        const uint32_t c = anim_color_from_hue(hue);
        const uint32_t r = (c >> 16) & 0xFF;
        const uint32_t g = (c >> 8) & 0xFF;
        const uint32_t b = c & 0xFF;
        zassert_true(r == 255 || g == 255 || b == 255, "hue %u = %06x has no 255 channel",
                     hue, c);
    }
}

// ---------------------------------------------------------------------------
// Spectrum sweep
// ---------------------------------------------------------------------------

ZTEST(color_mode_source, test_sweep_full_speed_period) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);
    raw.value = mode_value(0x01, 255);  // speed 255 -> 2000 ms full cycle

    zassert_equal(src.get(), 0xFF0000u);  // reset: phase 0 = red

    sNowMs += 500;  // quarter cycle: 1536/4 = hue 384
    zassert_equal(src.get(), anim_color_from_hue(384));

    sNowMs += 1500;  // completes the cycle: back to red
    zassert_equal(src.get(), 0xFF0000u);
}

ZTEST(color_mode_source, test_sweep_slowest_period) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);
    raw.value = mode_value(0x01, 0);  // speed 0 -> 2000 + 255*228 = 60140 ms

    zassert_equal(src.get(), 0xFF0000u);

    sNowMs += 30070;  // half cycle: hue 768 = cyan
    zassert_equal(src.get(), anim_color_from_hue(768));

    sNowMs += 30070;  // full cycle
    zassert_equal(src.get(), 0xFF0000u);
}

ZTEST(color_mode_source, test_sweep_speed_change_no_hue_jump) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);
    raw.value = mode_value(0x01, 255);

    (void)src.get();
    sNowMs += 500;
    zassert_equal(src.get(), anim_color_from_hue(384));

    // Same mode, new speed: NOT a state reset — with no time elapsed the hue
    // must be exactly where it was (phase advances incrementally).
    raw.value = mode_value(0x01, 0);
    zassert_equal(src.get(), anim_color_from_hue(384));
}

// ---------------------------------------------------------------------------
// Random on activation
// ---------------------------------------------------------------------------

ZTEST(color_mode_source, test_random_on_activate_holds_until_reactivated) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);
    raw.value = mode_value(0x03, 0);
    rng_script({100, 700});

    const uint16_t firstHue = roll_from(0, 100);
    zassert_equal(src.get(), anim_color_from_hue(firstHue));

    // Stable across ticks and time.
    sNowMs += 5000;
    zassert_equal(src.get(), anim_color_from_hue(firstHue));
    zassert_equal(src.get(), anim_color_from_hue(firstHue));

    src.notifyActivated();
    const uint16_t secondHue = roll_from(firstHue, 700);
    zassert_equal(src.get(), anim_color_from_hue(secondHue));
    zassert_not_equal(firstHue, secondHue);

    // Consecutive rolls are always >= 256 hue steps (60 degrees) apart.
    const uint16_t dist = (secondHue + 1536 - firstHue) % 1536;
    zassert_true(dist >= 256 && dist <= 1280, "hue jump %u out of range", dist);
}

// ---------------------------------------------------------------------------
// Random on beat
// ---------------------------------------------------------------------------

ZTEST(color_mode_source, test_random_on_beat_rolls_only_on_beat) {
    FakeRawSource raw;
    FakeBeatSource beats;
    ColorModeSource src(raw, scripted_rng, fake_now);
    src.setBeatSource(&beats);
    raw.value = mode_value(0x02, 0);
    rng_script({50, 900});

    const uint16_t firstHue = roll_from(0, 50);
    zassert_equal(src.get(), anim_color_from_hue(firstHue));  // never black pre-beat
    zassert_equal(src.get(), anim_color_from_hue(firstHue));

    beats.pending = true;
    const uint16_t secondHue = roll_from(firstHue, 900);
    zassert_equal(src.get(), anim_color_from_hue(secondHue));

    // Beat consumed: holds again until the next one.
    zassert_equal(src.get(), anim_color_from_hue(secondHue));
}

ZTEST(color_mode_source, test_random_on_beat_without_source_degrades_to_activate) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);  // no beat source bound
    raw.value = mode_value(0x02, 0);
    rng_script({300, 800});

    const uint16_t firstHue = roll_from(0, 300);
    zassert_equal(src.get(), anim_color_from_hue(firstHue));
    sNowMs += 10000;
    zassert_equal(src.get(), anim_color_from_hue(firstHue));

    // Re-activation still re-rolls, exactly like RandomOnActivate.
    src.notifyActivated();
    zassert_equal(src.get(), anim_color_from_hue(roll_from(firstHue, 800)));
}

// ---------------------------------------------------------------------------
// Random timer fade
// ---------------------------------------------------------------------------

ZTEST(color_mode_source, test_timer_fade_lerps_and_repicks) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);
    raw.value = mode_value(0x04, 255);  // speed 255 -> 1000 ms interval
    rng_script({100, 500, 200});

    // Reset: prev = roll(0, 100), target = roll(prev, 500).
    const uint16_t prevHue = roll_from(0, 100);
    const uint16_t targetHue = roll_from(prevHue, 500);
    const uint32_t prevColor = anim_color_from_hue(prevHue);
    const uint32_t targetColor = anim_color_from_hue(targetHue);

    zassert_equal(src.get(), prevColor);  // t = 0

    sNowMs += 500;  // midpoint: every channel between the two endpoints
    const uint32_t mid = src.get();
    for (int shift = 0; shift <= 16; shift += 8) {
        const uint32_t cm = (mid >> shift) & 0xFF;
        const uint32_t ca = (prevColor >> shift) & 0xFF;
        const uint32_t cb = (targetColor >> shift) & 0xFF;
        const uint32_t lo = ca < cb ? ca : cb;
        const uint32_t hi = ca < cb ? cb : ca;
        zassert_true(cm >= lo && cm <= hi, "midpoint channel %u outside [%u, %u]", cm, lo, hi);
    }

    sNowMs += 500;  // interval elapsed: segment rolls over, starts at old target
    zassert_equal(src.get(), targetColor);

    // And keeps moving toward the next pick.
    sNowMs += 1000;
    zassert_equal(src.get(), anim_color_from_hue(roll_from(targetHue, 200)));
}

ZTEST(color_mode_source, test_timer_fade_slowest_interval) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);
    raw.value = mode_value(0x04, 0);  // speed 0 -> 1000 + 255*114 = 30070 ms
    rng_script({100, 500});

    const uint16_t prevHue = roll_from(0, 100);
    const uint16_t targetHue = roll_from(prevHue, 500);

    zassert_equal(src.get(), anim_color_from_hue(prevHue));

    sNowMs += 30069;  // one ms short of the interval: still lerping
    zassert_not_equal(src.get(), anim_color_from_hue(targetHue));

    sNowMs += 1;  // interval complete
    zassert_equal(src.get(), anim_color_from_hue(targetHue));
}

// ---------------------------------------------------------------------------
// Mode transitions
// ---------------------------------------------------------------------------

ZTEST(color_mode_source, test_mode_switch_resets_state) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);

    // Sweep a quarter cycle...
    raw.value = mode_value(0x01, 255);
    (void)src.get();
    sNowMs += 500;
    zassert_equal(src.get(), anim_color_from_hue(384));

    // ...bounce through static...
    raw.value = 0x00123456;
    zassert_equal(src.get(), 0x00123456u);

    // ...and back to sweep: phase restarted at red, not resumed at 384.
    raw.value = mode_value(0x01, 255);
    zassert_equal(src.get(), 0xFF0000u);
}

ZTEST(color_mode_source, test_activation_reset_consumed_once) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);
    raw.value = mode_value(0x03, 0);
    rng_script({100, 700, 900});

    const uint16_t firstHue = roll_from(0, 100);
    zassert_equal(src.get(), anim_color_from_hue(firstHue));

    src.notifyActivated();
    src.notifyActivated();  // flag, not a counter: two arms, one reset
    const uint16_t secondHue = roll_from(firstHue, 700);
    zassert_equal(src.get(), anim_color_from_hue(secondHue));
    zassert_equal(src.get(), anim_color_from_hue(secondHue));  // no second roll
}
