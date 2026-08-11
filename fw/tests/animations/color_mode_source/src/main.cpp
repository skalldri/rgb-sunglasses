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
    uint32_t count = 0;
    uint32_t beatCount() override { return count; }
    void fire() { count++; }
};

// The hue a reset rolls starting from internal hue state 0 with rng value r:
// rollHueFrom(0) = (0 + 256 + r % 1024) % 1536.
constexpr uint16_t roll_from(uint16_t base, uint32_t r) {
    return static_cast<uint16_t>((base + 256u + (r % 1024u)) % 1536u);
}

constexpr uint32_t mode_value(uint8_t mode, uint8_t speed) {
    return (static_cast<uint32_t>(mode) << 24) | (static_cast<uint32_t>(speed) << 16);
}

// Halfway along the shorter hue arc — mirrors hue_lerp(from, to, 128) in the
// implementation, so the fade tests state the expected hue independently.
constexpr uint16_t hue_midpoint(uint16_t from, uint16_t to) {
    int32_t delta = static_cast<int32_t>(to) - static_cast<int32_t>(from);
    if (delta > 768) {
        delta -= 1536;
    } else if (delta < -768) {
        delta += 1536;
    }
    return static_cast<uint16_t>(
        ((static_cast<int32_t>(from) + (delta * 128) / 256) % 1536 + 1536) % 1536);
}

// The always-vivid invariant anim_color_from_hue() promises: at the pattern
// controller's ~2% global brightness, any color whose peak channel is below full
// scale is barely visible on the panel.
void assert_fully_vivid(uint32_t color, const char *what, uint32_t at) {
    const uint32_t r = (color >> 16) & 0xFFu;
    const uint32_t g = (color >> 8) & 0xFFu;
    const uint32_t b = color & 0xFFu;
    const uint32_t peak = r > g ? (r > b ? r : b) : (g > b ? g : b);
    zassert_equal(peak, 255u, "%s at %u: (%u,%u,%u) peaks at %u, not full scale", what, at, r, g,
                  b, peak);
}

// Two hues 768 units (180 degrees) apart on anim_color_from_hue()'s wheel are exact
// complements: each channel of one is 255 minus the corresponding channel of the other.
void assert_complementary(uint32_t a, uint32_t b, const char *when) {
    for (int shift = 16; shift >= 0; shift -= 8) {
        const uint32_t ca = (a >> shift) & 0xFFu;
        const uint32_t cb = (b >> shift) & 0xFFu;
        zassert_equal(ca + cb, 255u,
                      "%s: channel at shift %d is %u and %u, summing to %u rather than 255 "
                      "- the two sweeps are not half a wheel apart",
                      when, shift, ca, cb, ca + cb);
    }
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

    beats.fire();
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

    sNowMs += 500;  // midpoint: a real hue between the endpoints, still fully vivid
    const uint32_t mid = src.get();
    zassert_not_equal(mid, prevColor, "fade did not move off the previous color");
    zassert_not_equal(mid, targetColor, "fade reached the target early");
    zassert_equal(mid, anim_color_from_hue(hue_midpoint(prevHue, targetHue)),
                  "midpoint should be the hue-wheel midpoint, not an RGB blend");

    sNowMs += 500;  // interval elapsed: segment rolls over, starts at old target
    zassert_equal(src.get(), targetColor);

    // And keeps moving toward the next pick.
    sNowMs += 1000;
    zassert_equal(src.get(), anim_color_from_hue(roll_from(targetHue, 200)));
}

// Regression, hardware-reported (issue #259): the fade used to lerp the two
// endpoint colors per RGB channel, so each channel moved independently and the
// mid-fade slid through washed-out half-scale mid-tones — red -> green hit
// (128,127,0) and red -> cyan hit (128,127,127), i.e. grey at half brightness.
// On the panel that read as "red and green fade faster than blue" instead of one
// color turning into another. Every sample of the fade must stay fully vivid.
ZTEST(color_mode_source, test_timer_fade_stays_fully_vivid_across_the_whole_fade) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);
    raw.value = mode_value(0x04, 255);  // 1000 ms interval

    // Exercise a spread of arcs, including the 180-degree worst case for an RGB
    // blend (roll offsets are 256 + rng % 1024, so 512 -> a half-wheel jump).
    rng_script({0, 512, 100, 768, 900, 256, 300, 1000});
    zassert_equal(src.get(), anim_color_from_hue(roll_from(0, 0)));

    for (uint32_t segment = 0; segment < 4; segment++) {
        for (uint32_t step = 1; step <= 10; step++) {
            sNowMs += 100;
            assert_fully_vivid(src.get(), "timer fade", segment * 10 + step);
        }
    }
}

ZTEST(color_mode_source, test_timer_fade_walks_the_shorter_hue_arc) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);
    raw.value = mode_value(0x04, 255);  // 1000 ms interval
    // Offset 1279 (the largest a roll can produce) is 257 steps backwards, so the
    // fade must run down through 0 rather than 1279 steps forwards.
    rng_script({0, 1023});

    const uint16_t prevHue = roll_from(0, 0);          // 256
    const uint16_t targetHue = roll_from(prevHue, 1023);  // 256 + 1279 = 1535
    zassert_equal(src.get(), anim_color_from_hue(prevHue));

    sNowMs += 500;
    // Backwards half of a 257-step arc: 256 - 128 = 128, NOT forwards past 896.
    zassert_equal(src.get(), anim_color_from_hue(hue_midpoint(prevHue, targetHue)));
    zassert_equal(hue_midpoint(prevHue, targetHue), 128u);

    sNowMs += 500;
    zassert_equal(src.get(), anim_color_from_hue(targetHue));
}

// Both time-driven modes clamp a negative delta to zero. k_uptime_get() should
// never run backwards in production, but the clamps are the only thing standing
// between a backwards jump and a huge unsigned value in the phase/elapsed math,
// so prove they hold rather than trusting they are unreachable.
ZTEST(color_mode_source, test_backwards_clock_jump_is_clamped) {
    FakeRawSource raw;
    ColorModeSource src(raw, scripted_rng, fake_now);

    // SpectrumSweep: rewinding the clock must freeze the phase, not wrap it.
    raw.value = mode_value(0x01, 255);  // 2000 ms period
    sNowMs = 10000;
    zassert_equal(src.get(), anim_color_from_hue(0));
    sNowMs += 500;  // quarter of the period
    const uint32_t advanced = src.get();
    zassert_equal(advanced, anim_color_from_hue(384));

    sNowMs -= 5000;  // clock jumps backwards
    zassert_equal(src.get(), advanced, "backwards jump must hold the sweep phase");
    sNowMs += 500;  // and forward progress resumes normally from there
    zassert_equal(src.get(), anim_color_from_hue(768));

    // RandomTimerFade: same, on the segment-elapsed path.
    FakeRawSource fadeRaw;
    ColorModeSource fade(fadeRaw, scripted_rng, fake_now);
    fadeRaw.value = mode_value(0x04, 255);  // 1000 ms interval
    rng_script({0, 512});
    sNowMs = 10000;
    const uint16_t prevHue = roll_from(0, 0);
    zassert_equal(fade.get(), anim_color_from_hue(prevHue));
    sNowMs -= 5000;  // backwards before the segment has advanced at all
    zassert_equal(fade.get(), anim_color_from_hue(prevHue),
                  "backwards jump must not roll the segment over");
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

// ---------------------------------------------------------------------------
// Issue #344: several resolvers live at once
// ---------------------------------------------------------------------------

/* Bug 1: a shared beat feed must reach EVERY resolver.
 *
 * The old consume-once latch made this fail: whichever resolver resolved first cleared
 * the flag, so the second never saw the beat and its hue stayed frozen for the whole
 * session. An extension with two RandomOnBeat colours is the reachable case. */
ZTEST(color_mode_source, test_two_resolvers_both_observe_the_same_beat) {
    rng_script({10, 20, 30, 40});
    FakeBeatSource beats;

    FakeRawSource rawA;
    FakeRawSource rawB;
    rawA.value = mode_value(0x02, 0);  // RandomOnBeat
    rawB.value = mode_value(0x02, 0);
    ColorModeSource a(rawA, scripted_rng, fake_now);
    ColorModeSource b(rawB, scripted_rng, fake_now);
    a.setBeatSource(&beats);
    b.setBeatSource(&beats);

    // Settle both past their activation reset (which resyncs each cursor).
    const uint32_t a0 = a.get();
    const uint32_t b0 = b.get();

    beats.fire();

    // Resolution order only matters if the feed is destructive — which was the bug.
    const uint32_t a1 = a.get();
    const uint32_t b1 = b.get();

    zassert_not_equal(a1, a0, "First resolver should re-roll on the beat");
    zassert_not_equal(b1, b0, "Second resolver must see the same beat, not a consumed one");

    // With no further beats, neither moves again.
    zassert_equal(a.get(), a1, "No beat -> no re-roll");
    zassert_equal(b.get(), b1, "No beat -> no re-roll");
}

/* Beats counted while a resolver was idle must not fire on its activation. */
ZTEST(color_mode_source, test_beats_before_activation_are_not_reported) {
    rng_script({10, 20, 30});
    FakeBeatSource beats;
    beats.fire();
    beats.fire();

    FakeRawSource raw;
    raw.value = mode_value(0x02, 0);  // RandomOnBeat
    ColorModeSource s(raw, scripted_rng, fake_now);
    s.setBeatSource(&beats);

    const uint32_t first = s.get();
    zassert_equal(s.get(), first, "Pre-activation beats must not re-roll after the reset");

    beats.fire();
    zassert_not_equal(s.get(), first, "A beat after activation still re-rolls");
}

/* Bug 2: two SpectrumSweeps given distinct phase offsets must not be identical.
 *
 * Reset zeroes the phase and deliberately skips the random re-roll for this mode, so
 * without an offset both resolvers integrate the same clock into the same value forever
 * — and an animation interpolating between two equal endpoints renders a flat field,
 * which reads as the extension having hung. */
ZTEST(color_mode_source, test_offset_spectrum_sweeps_are_complementary) {
    FakeRawSource rawA;
    FakeRawSource rawB;
    rawA.value = mode_value(0x01, 255);  // SpectrumSweep, fastest
    rawB.value = mode_value(0x01, 255);
    // Ordinals 0 and 1 of two COLOR params — exactly plasma's layout once keyed on the
    // COLOR ordinal rather than the raw param index.
    ColorModeSource a(rawA, scripted_rng, fake_now, anim_sweep_phase_offset(0, 2));
    ColorModeSource b(rawB, scripted_rng, fake_now, anim_sweep_phase_offset(1, 2));

    // Asserting the SYMPTOM #344 is about (visibly distinct colours), not merely that
    // the offsets differ: zassert_not_equal would pass for a 1-unit hue gap as readily
    // as for 768, so any future narrowing of the spread would sail through it.
    //
    // Two hues half a wheel apart are exact complements on this 6-sector full-saturation
    // wheel: every sector's ramp is mirrored by the sector 768 units away, so the two
    // colours sum to 255 in every channel (red 255,0,0 vs cyan 0,255,255).
    assert_complementary(a.get(), b.get(), "at rest");

    // And the 180-degree relationship survives the sweep advancing, since both
    // integrate the same clock at the same rate.
    sNowMs += 500;
    assert_complementary(a.get(), b.get(), "after advancing");
}

/* Offset 0 is the default, so every built-in animation — one COLOR characteristic
 * each — is bit-for-bit unchanged by the constructor argument above. */
ZTEST(color_mode_source, test_default_sweep_offset_is_zero) {
    FakeRawSource rawA;
    FakeRawSource rawB;
    rawA.value = mode_value(0x01, 128);
    rawB.value = mode_value(0x01, 128);
    ColorModeSource explicitZero(rawA, scripted_rng, fake_now, anim_sweep_phase_offset(0, 16));
    ColorModeSource defaulted(rawB, scripted_rng, fake_now);

    zassert_equal(explicitZero.get(), defaulted.get(),
                  "Index 0 must equal the default, so built-ins are unchanged");

    sNowMs += 750;
    zassert_equal(explicitZero.get(), defaulted.get(), "and stay equal as the sweep advances");
}

/* The spread is evenly distributed and wraps to the full hue span, so callers can rely
 * on distinct indices producing distinct offsets. */
ZTEST(color_mode_source, test_sweep_phase_offset_spread) {
    zassert_equal(anim_sweep_phase_offset(0, 16), 0u, "Index 0 anchors at zero");
    zassert_true(anim_sweep_phase_offset(1, 16) < anim_sweep_phase_offset(2, 16),
                 "Offsets increase with index");
    zassert_equal(anim_sweep_phase_offset(8, 16), anim_sweep_phase_offset(1, 2),
                  "Halfway is halfway regardless of the divisor");

    // A single resolver anchors at zero: there is nothing to separate from, and this is
    // what keeps already-published single-colour extensions unchanged.
    zassert_equal(anim_sweep_phase_offset(0, 1), 0u, "Sole resolver anchors at zero");

    // index >= count must WRAP, not run off the end. index == count would return exactly
    // one full span, which the accumulator's own modulo reduces to phase 0 — silently
    // recreating the identical-sweeps bug this helper exists to prevent.
    zassert_equal(anim_sweep_phase_offset(1, 1), 0u, "index == count wraps to zero");
    zassert_equal(anim_sweep_phase_offset(17, 16), anim_sweep_phase_offset(1, 16),
                  "index past count wraps rather than exceeding a full span");
    zassert_equal(anim_sweep_phase_offset(1, 0), 0u,
                  "A zero count is treated as one rather than dividing by zero");
}
