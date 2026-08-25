/* Tests for the telemetry control-word decoder.
 *
 * This is the only place the telemetry service accepts untrusted input, and until this suite
 * existed none of its rejection paths had ever been observed rejecting anything — the same
 * shape of gap as the vacuous BtGattServer ordering comment and the vacuous param-table
 * self-check. Validation nobody has watched say no is not known to say no.
 *
 * The decoder is deliberately pure (audio_telemetry_control.h) so these run with no BT stack.
 * What is NOT covered here, because it is not decidable from the control word alone, is the
 * subscribe-before-arm rule and the hold/rate re-assert edges — those need isSubscribed() and
 * the service's own static state, and live in audio_telemetry_service.cpp.
 */

#include <zephyr/ztest.h>

#include "audio_telemetry_control.h"

namespace {

/* Mirrors what audio_telemetry_service.cpp passes. Kept as literals rather than reaching for
 * the CONFIG_* symbols because this test project does not pull in fw/Kconfig; the Kconfig
 * `range` clauses independently keep the real values inside these bounds, and the
 * default_outside_range test below proves the decoder survives it even if they did not. */
constexpr uint8_t kDefaultRate = 8;
constexpr uint16_t kDefaultHold = 60;
constexpr uint16_t kMinHold = 5;
constexpr uint16_t kMaxHold = 255;

audio_telemetry_control parse(uint32_t control) {
    return audio_telemetry_control_parse(control, kDefaultRate, kDefaultHold, kMinHold, kMaxHold);
}

/* Assembles a control word the way the app does, so a test reads as intent rather than shifts. */
constexpr uint32_t ctrl(uint8_t tier, uint8_t rate, uint8_t hold, uint8_t reserved = 0) {
    return (uint32_t)tier | ((uint32_t)rate << 8) | ((uint32_t)hold << 16) |
           ((uint32_t)reserved << 24);
}

} // namespace

ZTEST_SUITE(audio_telemetry_control, nullptr, nullptr, nullptr, nullptr, nullptr);

/* ---------- rejection ---------- */

ZTEST(audio_telemetry_control, test_reserved_bits_rejected) {
    /* Every reserved bit individually, not just the byte as a whole: a mask typo that let one
     * bit through would pass a whole-byte test that only ever sets 0xFF. */
    for (int bit = 24; bit < 32; bit++) {
        const uint32_t control = ctrl(AUDIO_TELEMETRY_TIER_METERS, 8, 60) | (1u << bit);
        const audio_telemetry_control out = parse(control);
        zassert_false(out.valid, "reserved bit %d accepted", bit);
        zassert_equal(out.error, -EINVAL, "reserved bit %d gave %d", bit, out.error);
    }
}

ZTEST(audio_telemetry_control, test_reserved_bits_rejected_even_when_rest_is_valid) {
    /* The reserved check must come FIRST. If it were folded in after the tier check, a word
     * that is otherwise a perfectly good "stop" would be accepted with unknown bits set. */
    const audio_telemetry_control out = parse(ctrl(AUDIO_TELEMETRY_TIER_OFF, 0, 0, 0x01));
    zassert_false(out.valid, "reserved bits accepted alongside a valid tier");
    zassert_equal(out.error, -EINVAL);
}

ZTEST(audio_telemetry_control, test_tier_above_max_rejected) {
    for (uint32_t tier = AUDIO_TELEMETRY_TIER_MAX + 1; tier <= 0xFF; tier++) {
        const audio_telemetry_control out = parse(ctrl((uint8_t)tier, 8, 60));
        zassert_false(out.valid, "tier %u accepted", tier);
        zassert_equal(out.error, -EINVAL, "tier %u gave %d", tier, out.error);
    }
}

ZTEST(audio_telemetry_control, test_every_defined_tier_accepted) {
    for (uint32_t tier = 0; tier <= AUDIO_TELEMETRY_TIER_MAX; tier++) {
        const audio_telemetry_control out = parse(ctrl((uint8_t)tier, 8, 60));
        zassert_true(out.valid, "tier %u rejected", tier);
        zassert_equal(out.tier, tier);
    }
}

/* ---------- stop ---------- */

ZTEST(audio_telemetry_control, test_off_is_accepted_with_junk_rate_and_hold) {
    /* A stop must never fail on the fields it does not use. The app sends control=0 to stop,
     * but a caller re-sending its last word with the tier zeroed must stop too — otherwise the
     * one command that reliably quiets the radio could be refused. */
    const audio_telemetry_control out = parse(ctrl(AUDIO_TELEMETRY_TIER_OFF, 200, 255));
    zassert_true(out.valid);
    zassert_equal(out.tier, AUDIO_TELEMETRY_TIER_OFF);
}

/* ---------- rate ---------- */

ZTEST(audio_telemetry_control, test_rate_zero_takes_the_default) {
    const audio_telemetry_control out = parse(ctrl(AUDIO_TELEMETRY_TIER_METERS, 0, 60));
    zassert_true(out.valid);
    zassert_equal(out.rate_hz, kDefaultRate, "got %u", out.rate_hz);
}

ZTEST(audio_telemetry_control, test_rate_clamped_to_analysis_rate) {
    /* The DSP produces 31.25 frames/s. Asking for 200 Hz is not a request that can be honoured,
     * so it is clamped rather than believed — a tick scheduled at 5 ms would just spin. */
    for (uint32_t rate = AUDIO_TELEMETRY_CTRL_MAX_RATE_HZ + 1; rate <= 0xFF; rate++) {
        const audio_telemetry_control out = parse(ctrl(AUDIO_TELEMETRY_TIER_METERS, (uint8_t)rate, 60));
        zassert_true(out.valid, "rate %u rejected", rate);
        zassert_equal(out.rate_hz, AUDIO_TELEMETRY_CTRL_MAX_RATE_HZ, "rate %u -> %u", rate,
                      out.rate_hz);
    }
}

ZTEST(audio_telemetry_control, test_rate_passes_through_in_band) {
    for (uint32_t rate = 1; rate <= AUDIO_TELEMETRY_CTRL_MAX_RATE_HZ; rate++) {
        const audio_telemetry_control out = parse(ctrl(AUDIO_TELEMETRY_TIER_SPECTRUM, (uint8_t)rate, 60));
        zassert_true(out.valid, "rate %u rejected", rate);
        zassert_equal(out.rate_hz, rate, "rate %u -> %u", rate, out.rate_hz);
    }
}

ZTEST(audio_telemetry_control, test_wizard_burst_rate_survives) {
    /* The tap-along step asks for undecimated frames (31.25 Hz rounded up to 32) at tier STATS,
     * so it can replay the detector's window exactly. If this were clamped the offline sweep
     * would silently fit against decimated stats. */
    const audio_telemetry_control out = parse(ctrl(AUDIO_TELEMETRY_TIER_STATS, 32, 60));
    zassert_true(out.valid);
    zassert_equal(out.rate_hz, 32);
    zassert_equal(out.tier, AUDIO_TELEMETRY_TIER_STATS);
}

ZTEST(audio_telemetry_control, test_zero_default_rate_never_yields_zero) {
    /* A rate of 0 reaches the tick as a 1000/rate division and as the governor's FAST/MEDIUM
     * discriminator. Nothing downstream should have to defend against it. */
    const audio_telemetry_control out =
        audio_telemetry_control_parse(ctrl(AUDIO_TELEMETRY_TIER_METERS, 0, 60), 0, kDefaultHold,
                                      kMinHold, kMaxHold);
    zassert_true(out.valid);
    zassert_equal(out.rate_hz, 1, "a zero default leaked through as %u", out.rate_hz);
}

/* ---------- hold ---------- */

ZTEST(audio_telemetry_control, test_hold_zero_takes_the_default) {
    const audio_telemetry_control out = parse(ctrl(AUDIO_TELEMETRY_TIER_METERS, 8, 0));
    zassert_true(out.valid);
    zassert_equal(out.hold_s, kDefaultHold, "got %u", out.hold_s);
}

ZTEST(audio_telemetry_control, test_hold_clamped_up_to_minimum) {
    /* The app re-arms every hold_s/2. A 1 s hold would mean a 500 ms re-arm cadence — more ATT
     * traffic than the stream itself, and the watchdog would trip on any scheduling hiccup. */
    for (uint32_t hold = 1; hold < kMinHold; hold++) {
        const audio_telemetry_control out = parse(ctrl(AUDIO_TELEMETRY_TIER_METERS, 8, (uint8_t)hold));
        zassert_true(out.valid, "hold %u rejected", hold);
        zassert_equal(out.hold_s, kMinHold, "hold %u -> %u", hold, out.hold_s);
    }
}

ZTEST(audio_telemetry_control, test_hold_clamped_down_to_configured_maximum) {
    const uint16_t configured_max = 90;
    const audio_telemetry_control out = audio_telemetry_control_parse(
        ctrl(AUDIO_TELEMETRY_TIER_METERS, 8, 255), kDefaultRate, kDefaultHold, kMinHold,
        configured_max);
    zassert_true(out.valid);
    zassert_equal(out.hold_s, configured_max, "got %u", out.hold_s);
}

ZTEST(audio_telemetry_control, test_hold_cannot_exceed_the_field_width) {
    /* CONFIG_APP_AUDIO_TELEMETRY_MAX_HOLD_S was 300 until review, which the 8-bit field could
     * never deliver. The guarantee now comes from the field itself rather than a clamp: even
     * asked for the largest word the format can carry, against a maximum well above it, the
     * result cannot exceed 255. Written as a sweep over the whole field so it would fail if the
     * extraction mask ever widened past 8 bits and made the Kconfig range meaningful again. */
    for (uint32_t hold = 0; hold <= 0xFF; hold++) {
        const audio_telemetry_control out = audio_telemetry_control_parse(
            ctrl(AUDIO_TELEMETRY_TIER_METERS, 8, (uint8_t)hold), kDefaultRate, kDefaultHold,
            kMinHold, 4000 /* far above the field width */);
        zassert_true(out.valid, "hold %u rejected", hold);
        zassert_true(out.hold_s <= AUDIO_TELEMETRY_CTRL_MAX_HOLD_S, "hold %u -> %u", hold,
                     out.hold_s);
    }
}

ZTEST(audio_telemetry_control, test_hold_passes_through_in_band) {
    for (uint32_t hold = kMinHold; hold <= kMaxHold; hold++) {
        const audio_telemetry_control out = parse(ctrl(AUDIO_TELEMETRY_TIER_METERS, 8, (uint8_t)hold));
        zassert_true(out.valid, "hold %u rejected", hold);
        zassert_equal(out.hold_s, hold, "hold %u -> %u", hold, out.hold_s);
    }
}

ZTEST(audio_telemetry_control, test_default_outside_range_is_still_clamped) {
    /* Defaulting and clamping are separate steps in that order, so a misconfigured default is
     * corrected rather than trusted. Kconfig `range` should prevent this; belt and braces,
     * because the failure mode is a device that streams for an hour with nobody watching. */
    const audio_telemetry_control out = audio_telemetry_control_parse(
        ctrl(AUDIO_TELEMETRY_TIER_METERS, 0, 0), 200 /* rate */, 1 /* hold */, kMinHold, kMaxHold);
    zassert_true(out.valid);
    zassert_equal(out.rate_hz, AUDIO_TELEMETRY_CTRL_MAX_RATE_HZ, "got %u", out.rate_hz);
    zassert_equal(out.hold_s, kMinHold, "got %u", out.hold_s);
}

/* ---------- assembly ---------- */

ZTEST(audio_telemetry_control, test_fields_do_not_bleed_into_each_other) {
    /* Distinct values in all three fields, none of them shared, so a wrong shift or mask shows
     * up as a field carrying its neighbour's value rather than as a plausible number. */
    const audio_telemetry_control out = parse(ctrl(AUDIO_TELEMETRY_TIER_SPECTRUM, 17, 42));
    zassert_true(out.valid);
    zassert_equal(out.tier, AUDIO_TELEMETRY_TIER_SPECTRUM);
    zassert_equal(out.rate_hz, 17);
    zassert_equal(out.hold_s, 42);
}

ZTEST(audio_telemetry_control, test_invalid_word_leaves_output_inert) {
    /* onWriteChecked returns before touching s_tier on a rejection, but a caller that read the
     * fields anyway must not find a live-looking stream request in them. */
    const audio_telemetry_control out = parse(ctrl(0xFF, 32, 255));
    zassert_false(out.valid);
    zassert_equal(out.tier, AUDIO_TELEMETRY_TIER_OFF);
    zassert_equal(out.rate_hz, 0);
    zassert_equal(out.hold_s, 0);
}
