/* Unit tests for the live audio telemetry wire format (fw/src/sound/audio_telemetry_codec.h).
 *
 * This format is a compatibility surface in two directions at once: the firmware packs it and
 * the companion app unpacks it, and the two ship independently. The properties worth pinning
 * are therefore the ones a reader cannot check by eye:
 *
 *   - the meters tier is EXACTLY 20 bytes, because that is ATT_MTU 23 minus the notify header
 *     and bt_gatt_notify() cannot fragment;
 *   - each tier is a byte-exact PREFIX of the next, so a consumer can parse what it
 *     understands and ignore the rest;
 *   - the quantiser round-trips within its stated 0.5 dB, including at both saturation ends
 *     and for zero.
 *
 * Header-only and Zephyr-free, so this suite links nothing.
 */
#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>

#include "audio_telemetry_codec.h"

namespace {

/* A frame with every field set to something distinguishable, so a packing mistake that
 * swaps two fields shows up as a value mismatch rather than passing by coincidence. */
struct audio_telemetry_frame make_frame() {
    struct audio_telemetry_frame f;
    memset(&f, 0, sizeof(f));

    f.seq = 0xBEEF;
    f.dropped = 7;
    f.gain_steps = 18; /* +9 dB */
    f.rms_input_referred = 0.0007f;
    f.rms_instant = 0.0011f;
    f.peak = 0.0035f;
    f.noise_floor = 0.0004f;
    f.clip_count = 3;
    f.frames_since_step = 42;
    f.silent = true;
    f.clipped = false;
    f.agc_frozen = true;
    f.threshold_mode = 1;
    f.beat_mask = 0x5; /* bands 0 and 2 */

    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        f.flux[b] = 0.10f * (float)(b + 1);
        f.threshold[b] = 0.20f * (float)(b + 1);
        f.mean[b] = 0.30f * (float)(b + 1);
        f.sigma[b] = 0.40f * (float)(b + 1);
    }
    for (int i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) {
        f.buckets[i] = 0.01f * (float)(i + 1);
    }
    return f;
}

/* Quantised magnitudes are only good to 0.5 dB, i.e. a factor of 10^0.025. Comparing them
 * with an absolute epsilon would either be vacuous for large values or fail for small ones. */
bool within_half_db(float got, float want) {
    if (want == 0.0f) {
        return got == 0.0f;
    }
    const float ratio = got / want;
    return ratio > 0.941f && ratio < 1.060f;
}

}  // namespace

ZTEST_SUITE(audio_telemetry_codec, NULL, NULL, NULL, NULL, NULL);

/* ── Sizes and the MTU-23 guarantee ──────────────────────────────────────── */

ZTEST(audio_telemetry_codec, test_tier_sizes_are_exact) {
    zassert_equal(audio_telemetry_tier_size(AUDIO_TELEMETRY_TIER_METERS), 20);
    zassert_equal(audio_telemetry_tier_size(AUDIO_TELEMETRY_TIER_STATS), 28);
    zassert_equal(audio_telemetry_tier_size(AUDIO_TELEMETRY_TIER_SPECTRUM), 48);
    zassert_equal(audio_telemetry_tier_size(AUDIO_TELEMETRY_TIER_OFF), 0);
    zassert_equal(audio_telemetry_tier_size(99), 0, "unknown tiers must report 0, not guess");
}

ZTEST(audio_telemetry_codec, test_meters_tier_survives_an_unnegotiated_mtu) {
    /* The reason the meters tier is shaped the way it is. bt_gatt_notify() cannot fragment,
     * so one byte over this and a degraded link goes silent instead of degrading. */
    zassert_equal(AUDIO_TELEMETRY_SIZE_METERS, 20);
    zassert_true(AUDIO_TELEMETRY_SIZE_METERS <= AUDIO_TELEMETRY_UNNEGOTIATED_ATT_PAYLOAD);
}

ZTEST(audio_telemetry_codec, test_tier_for_mtu_clamps_down_never_up) {
    /* MTU 23 -> 20 usable: meters only. */
    zassert_equal(audio_telemetry_tier_for_mtu(AUDIO_TELEMETRY_TIER_SPECTRUM, 20),
                  AUDIO_TELEMETRY_TIER_METERS);
    zassert_equal(audio_telemetry_tier_for_mtu(AUDIO_TELEMETRY_TIER_STATS, 20),
                  AUDIO_TELEMETRY_TIER_METERS);
    /* Enough for stats but not spectrum. */
    zassert_equal(audio_telemetry_tier_for_mtu(AUDIO_TELEMETRY_TIER_SPECTRUM, 28),
                  AUDIO_TELEMETRY_TIER_STATS);
    /* A healthy link gets what it asked for, and never more. */
    zassert_equal(audio_telemetry_tier_for_mtu(AUDIO_TELEMETRY_TIER_SPECTRUM, 495),
                  AUDIO_TELEMETRY_TIER_SPECTRUM);
    zassert_equal(audio_telemetry_tier_for_mtu(AUDIO_TELEMETRY_TIER_METERS, 495),
                  AUDIO_TELEMETRY_TIER_METERS);
    /* Below even the meters tier there is nothing honest to send. */
    zassert_equal(audio_telemetry_tier_for_mtu(AUDIO_TELEMETRY_TIER_SPECTRUM, 19),
                  AUDIO_TELEMETRY_TIER_OFF);
    /* A nonsense request is clamped to the top tier, not trusted. */
    zassert_equal(audio_telemetry_tier_for_mtu(200, 495), AUDIO_TELEMETRY_TIER_SPECTRUM);
}

/* ── The prefix property ─────────────────────────────────────────────────── */

ZTEST(audio_telemetry_codec, test_each_tier_is_a_byte_exact_prefix_of_the_next) {
    /* This is what lets an older app parse a newer device's frame. If it ever stops
     * holding, every consumer has to branch on tier before reading ANY field. */
    const struct audio_telemetry_frame f = make_frame();
    uint8_t meters[64], stats[64], spectrum[64];

    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_METERS, meters, sizeof(meters)),
                  20);
    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_STATS, stats, sizeof(stats)), 28);
    zassert_equal(
        audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_SPECTRUM, spectrum, sizeof(spectrum)), 48);

    /* Byte 0 legitimately differs — it records the tier actually sent. Everything after it
     * must be identical. */
    zassert_mem_equal(meters + 1, stats + 1, 19, "stats tier diverges from meters inside byte 20");
    zassert_mem_equal(stats + 1, spectrum + 1, 27, "spectrum tier diverges inside byte 28");
}

ZTEST(audio_telemetry_codec, test_header_records_the_tier_actually_sent) {
    const struct audio_telemetry_frame f = make_frame();
    uint8_t buf[64];

    for (uint8_t tier = AUDIO_TELEMETRY_TIER_METERS; tier <= AUDIO_TELEMETRY_TIER_MAX; tier++) {
        zassert_true(audio_telemetry_pack(&f, tier, buf, sizeof(buf)) > 0);
        zassert_equal(audio_telemetry_packed_version(buf), AUDIO_TELEMETRY_VERSION);
        zassert_equal(audio_telemetry_packed_tier(buf), tier,
                      "a consumer must never have to infer the tier from the length");
    }
}

/* ── Round-trip ──────────────────────────────────────────────────────────── */

ZTEST(audio_telemetry_codec, test_round_trip_preserves_every_meters_field) {
    const struct audio_telemetry_frame f = make_frame();
    uint8_t buf[64];
    struct audio_telemetry_frame got;

    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_METERS, buf, sizeof(buf)), 20);
    zassert_true(audio_telemetry_unpack(buf, 20, &got));

    /* Exact fields: anything that is not a quantised magnitude must survive bit-for-bit. */
    zassert_equal(got.seq, f.seq);
    zassert_equal(got.dropped, f.dropped);
    zassert_equal(got.gain_steps, f.gain_steps, "gain is lossless — the register step IS 0.5 dB");
    zassert_equal(got.clip_count, f.clip_count);
    zassert_equal(got.frames_since_step, f.frames_since_step);
    zassert_equal(got.silent, f.silent);
    zassert_equal(got.clipped, f.clipped);
    zassert_equal(got.agc_frozen, f.agc_frozen);
    zassert_equal(got.threshold_mode, f.threshold_mode);
    zassert_equal(got.beat_mask, f.beat_mask, "beat bits are authoritative and must be exact");

    /* Quantised fields: within the stated 0.5 dB. */
    zassert_true(within_half_db(got.rms_input_referred, f.rms_input_referred));
    zassert_true(within_half_db(got.rms_instant, f.rms_instant));
    zassert_true(within_half_db(got.peak, f.peak));
    zassert_true(within_half_db(got.noise_floor, f.noise_floor));
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        zassert_true(within_half_db(got.flux[b], f.flux[b]), "flux band %d", b);
        zassert_true(within_half_db(got.threshold[b], f.threshold[b]), "threshold band %d", b);
    }
}

ZTEST(audio_telemetry_codec, test_higher_tiers_add_their_fields) {
    const struct audio_telemetry_frame f = make_frame();
    uint8_t buf[64];
    struct audio_telemetry_frame got;

    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_STATS, buf, sizeof(buf)), 28);
    zassert_true(audio_telemetry_unpack(buf, 28, &got));
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        zassert_true(within_half_db(got.mean[b], f.mean[b]), "mean band %d", b);
        zassert_true(within_half_db(got.sigma[b], f.sigma[b]), "sigma band %d", b);
    }
    /* Buckets are not in this tier and must read as absent, not as stale garbage. */
    for (int i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) {
        zassert_equal(got.buckets[i], 0.0f);
    }

    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_SPECTRUM, buf, sizeof(buf)), 48);
    zassert_true(audio_telemetry_unpack(buf, 48, &got));
    for (int i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) {
        zassert_true(within_half_db(got.buckets[i], f.buckets[i]), "bucket %d", i);
    }
}

ZTEST(audio_telemetry_codec, test_meters_tier_zeroes_the_fields_it_does_not_carry) {
    /* A consumer must be able to tell "this tier has no stats" from "the stats were zero"
     * by looking at the tier, and must never see uninitialised memory either way. */
    const struct audio_telemetry_frame f = make_frame();
    uint8_t buf[64];
    struct audio_telemetry_frame got;

    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_METERS, buf, sizeof(buf)), 20);
    zassert_true(audio_telemetry_unpack(buf, 20, &got));
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        zassert_equal(got.mean[b], 0.0f);
        zassert_equal(got.sigma[b], 0.0f);
    }
}

/* ── Quantiser ───────────────────────────────────────────────────────────── */

ZTEST(audio_telemetry_codec, test_q_log_anchors) {
    zassert_equal(audio_telemetry_q_log(1.0f), 200, "0 dBFS must land on q=200");
    zassert_equal(audio_telemetry_q_log(0.0f), 0);
    zassert_equal(audio_telemetry_dq_log(0), 0.0f);
    zassert_true(within_half_db(audio_telemetry_dq_log(200), 1.0f));
}

ZTEST(audio_telemetry_codec, test_q_log_round_trips_every_code) {
    /* q -> value -> q must be the identity across the whole ladder, or a value would drift
     * a step every time it passed through a device that re-encoded it. */
    for (int q = 1; q <= 255; q++) {
        const float v = audio_telemetry_dq_log((uint8_t)q);
        zassert_equal(audio_telemetry_q_log(v), q, "q=%d round-tripped to %d", q,
                      audio_telemetry_q_log(v));
    }
}

ZTEST(audio_telemetry_codec, test_q_log_is_monotonic) {
    for (int q = 1; q < 255; q++) {
        zassert_true(audio_telemetry_dq_log((uint8_t)(q + 1)) > audio_telemetry_dq_log((uint8_t)q),
                     "ladder must increase at q=%d", q);
    }
}

ZTEST(audio_telemetry_codec, test_q_log_saturates_rather_than_wrapping) {
    zassert_equal(audio_telemetry_q_log(1.0e9f), 255, "above the ceiling must pin, not wrap");
    zassert_equal(audio_telemetry_q_log(1.0e-30f), 0, "below the floor reads as zero");
    zassert_equal(audio_telemetry_q_log(-1.0f), 0, "negative magnitudes are not a thing");
}

ZTEST(audio_telemetry_codec, test_q_log_rejects_non_finite) {
    /* A NaN reaching a meter renders as a blank or a wild spike; there is no honest value
     * to show, so it maps to "nothing". */
    zassert_equal(audio_telemetry_q_log(NAN), 0);
    zassert_equal(audio_telemetry_q_log(INFINITY), 255);
    zassert_equal(audio_telemetry_q_log(-INFINITY), 0);
}

ZTEST(audio_telemetry_codec, test_q_log_covers_the_measured_operating_range) {
    /* The range was chosen against real measurements, not guessed. These are the numbers
     * from the bench: room noise at 0 dB gain, and the largest band-0 flux seen on music. */
    const float kRoomNoise = 0.0006f;
    const float kMusicFluxPeak = 3.5f;
    zassert_true(audio_telemetry_q_log(kRoomNoise) > 1, "room noise must not sit on the floor");
    zassert_true(audio_telemetry_q_log(kMusicFluxPeak) < 255, "music flux must not saturate");
    zassert_true(
        within_half_db(audio_telemetry_dq_log(audio_telemetry_q_log(kRoomNoise)), kRoomNoise));
    zassert_true(within_half_db(audio_telemetry_dq_log(audio_telemetry_q_log(kMusicFluxPeak)),
                                kMusicFluxPeak));
}

/* ── Refusals ────────────────────────────────────────────────────────────── */

ZTEST(audio_telemetry_codec, test_pack_refuses_bad_arguments) {
    const struct audio_telemetry_frame f = make_frame();
    uint8_t buf[64];

    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_OFF, buf, sizeof(buf)), 0);
    zassert_equal(audio_telemetry_pack(&f, 99, buf, sizeof(buf)), 0);
    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_METERS, buf, 19), 0,
                  "a short buffer must be refused, never partially written");
    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_SPECTRUM, buf, 47), 0);
    zassert_equal(audio_telemetry_pack(NULL, AUDIO_TELEMETRY_TIER_METERS, buf, sizeof(buf)), 0);
    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_METERS, NULL, 64), 0);
}

ZTEST(audio_telemetry_codec, test_unpack_refuses_a_version_it_does_not_know) {
    /* Misparsing a future layout would be worse than showing nothing. */
    const struct audio_telemetry_frame f = make_frame();
    uint8_t buf[64];
    struct audio_telemetry_frame got;

    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_METERS, buf, sizeof(buf)), 20);
    buf[0] = (uint8_t)((0x0F << 4) | AUDIO_TELEMETRY_TIER_METERS);
    zassert_false(audio_telemetry_unpack(buf, 20, &got));
}

ZTEST(audio_telemetry_codec, test_unpack_refuses_short_and_malformed_buffers) {
    const struct audio_telemetry_frame f = make_frame();
    uint8_t buf[64];
    struct audio_telemetry_frame got;

    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_SPECTRUM, buf, sizeof(buf)), 48);
    zassert_false(audio_telemetry_unpack(buf, 47, &got), "truncated frames must be refused");
    zassert_false(audio_telemetry_unpack(buf, 0, &got));
    zassert_false(audio_telemetry_unpack(NULL, 48, &got));
    zassert_false(audio_telemetry_unpack(buf, 48, NULL));

    /* An unknown tier in an otherwise valid header. */
    buf[0] = (uint8_t)((AUDIO_TELEMETRY_VERSION << 4) | 0x0E);
    zassert_false(audio_telemetry_unpack(buf, 48, &got));
}

ZTEST(audio_telemetry_codec, test_unpack_tolerates_a_longer_buffer_than_the_tier) {
    /* Forward compatibility: a newer device may send a longer frame at a tier this build
     * understands. Trailing bytes are not this consumer's business. */
    const struct audio_telemetry_frame f = make_frame();
    uint8_t buf[64];
    struct audio_telemetry_frame got;

    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_METERS, buf, sizeof(buf)), 20);
    zassert_true(audio_telemetry_unpack(buf, 64, &got));
    zassert_equal(got.seq, f.seq);
}

/* ── Field-level packing details ─────────────────────────────────────────── */

ZTEST(audio_telemetry_codec, test_seq_is_little_endian_like_every_other_characteristic) {
    struct audio_telemetry_frame f = make_frame();
    f.seq = 0x1234;
    uint8_t buf[64];

    zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_METERS, buf, sizeof(buf)), 20);
    zassert_equal(buf[2], 0x34);
    zassert_equal(buf[3], 0x12);
}

ZTEST(audio_telemetry_codec, test_negative_gain_survives_the_signed_round_trip) {
    struct audio_telemetry_frame f = make_frame();
    uint8_t buf[64];
    struct audio_telemetry_frame got;

    /* The register spans 0x00..0x50 around a 0x28 park, so the relative value is genuinely
     * signed: -40 .. +40 steps, i.e. -20 .. +20 dB. */
    for (int steps = -40; steps <= 40; steps++) {
        f.gain_steps = (int8_t)steps;
        zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_METERS, buf, sizeof(buf)), 20);
        zassert_true(audio_telemetry_unpack(buf, 20, &got));
        zassert_equal(got.gain_steps, steps, "gain %d steps did not survive", steps);
    }
}

ZTEST(audio_telemetry_codec, test_every_flag_and_beat_bit_is_independent) {
    /* A shift or mask error here would couple two unrelated states — e.g. "silent" turning
     * a beat bit on — which is exactly the kind of thing that reads as a detector bug. */
    struct audio_telemetry_frame f;
    uint8_t buf[64];
    struct audio_telemetry_frame got;

    for (int mask = 0; mask < 16; mask++) {
        memset(&f, 0, sizeof(f));
        f.beat_mask = (uint8_t)mask;
        zassert_equal(audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_METERS, buf, sizeof(buf)), 20);
        zassert_true(audio_telemetry_unpack(buf, 20, &got));
        zassert_equal(got.beat_mask, mask);
        zassert_false(got.silent);
        zassert_false(got.clipped);
        zassert_false(got.agc_frozen);
        zassert_equal(got.threshold_mode, 0);
    }

    const bool values[2] = {false, true};
    for (int s = 0; s < 2; s++) {
        for (int c = 0; c < 2; c++) {
            for (int z = 0; z < 2; z++) {
                for (int m = 0; m < 2; m++) {
                    memset(&f, 0, sizeof(f));
                    f.silent = values[s];
                    f.clipped = values[c];
                    f.agc_frozen = values[z];
                    f.threshold_mode = (uint8_t)m;
                    zassert_equal(
                        audio_telemetry_pack(&f, AUDIO_TELEMETRY_TIER_METERS, buf, sizeof(buf)),
                        20);
                    zassert_true(audio_telemetry_unpack(buf, 20, &got));
                    zassert_equal(got.silent, values[s]);
                    zassert_equal(got.clipped, values[c]);
                    zassert_equal(got.agc_frozen, values[z]);
                    zassert_equal(got.threshold_mode, (uint8_t)m);
                    zassert_equal(got.beat_mask, 0, "flags must not leak into the beat nibble");
                }
            }
        }
    }
}
