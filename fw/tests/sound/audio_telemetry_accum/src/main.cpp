/* Unit tests for the live telemetry accumulator (fw/src/sound/audio_telemetry.cpp).
 *
 * Analysis runs at 31.25 Hz and the phone is sent frames at a lower rate, so most frames
 * are folded away. The property that matters most is that the folding is not lossy in the
 * ways that would lie about the room: a beat in a dropped frame must still be reported, and
 * a peak from a dropped frame must still be the window's peak. Getting that wrong shows up
 * as a missed flash and as a corrupted tap-agreement measurement in the calibration wizard
 * — both of which read as detector bugs rather than transport bugs.
 */
#include <string.h>
#include <zephyr/ztest.h>

#include "audio_telemetry.h"

namespace {

/* A frame with distinguishable per-band values so a band mix-up cannot pass by luck. */
struct audio_analysis_result make_result(uint32_t seq) {
    struct audio_analysis_result r;
    memset(&r, 0, sizeof(r));
    r.seq = seq;
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        r.band_flux[b] = 0.10f * (float)(b + 1);
        r.band_mean[b] = 0.20f * (float)(b + 1);
        r.band_sigma[b] = 0.05f * (float)(b + 1);
        r.beat[b] = false;
    }
    for (int i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) {
        r.display_bucket_energy[i] = 0.01f * (float)(i + 1);
    }
    return r;
}

/* Publish with sensible defaults, so each test only states what it is actually about. */
void publish(const struct audio_analysis_result &r, float peak = 0.01f, bool clipped = false,
             uint32_t mode = 0, float alpha = 0.3f, float floor_v = 0.0f,
             uint32_t frames_since_step = 0) {
    audio_telemetry_publish(&r, 0.002f, 0.003f, peak, 0.0006f, 18, frames_since_step,
                            /* silent */ false, clipped, /* frozen */ false, mode, alpha, floor_v);
}

void setup_active() {
    audio_telemetry_reset();
    audio_telemetry_set_active(true);
}

}  // namespace

ZTEST_SUITE(audio_telemetry_accum, NULL, NULL, NULL, NULL, NULL);

/* ── The property this whole design exists for ───────────────────────────── */

ZTEST(audio_telemetry_accum, test_a_beat_in_a_decimated_away_frame_still_reports) {
    setup_active();

    /* Four analysis frames, one send — the normal 31.25 Hz -> 8 Hz case. The beat lands in
     * the second frame, which will never be the one whose values are sent. */
    struct audio_analysis_result r = make_result(1);
    publish(r);

    r = make_result(2);
    r.beat[2] = true;
    publish(r);

    r = make_result(3);
    publish(r);
    r = make_result(4);
    publish(r);

    struct audio_telemetry_frame out;
    zassert_true(audio_telemetry_take(&out));
    zassert_equal(out.seq, 4, "latest frame wins for levels");
    zassert_equal(out.beat_mask, 1u << 2,
                  "a beat from a folded-away frame must survive — issue #376's whole point");
}

ZTEST(audio_telemetry_accum, test_beats_or_across_all_bands_in_the_window) {
    setup_active();

    struct audio_analysis_result r = make_result(1);
    r.beat[0] = true;
    publish(r);
    r = make_result(2);
    r.beat[3] = true;
    publish(r);

    struct audio_telemetry_frame out;
    zassert_true(audio_telemetry_take(&out));
    zassert_equal(out.beat_mask, (1u << 0) | (1u << 3));
}

ZTEST(audio_telemetry_accum, test_beat_flags_do_not_leak_into_the_next_window) {
    setup_active();

    struct audio_analysis_result r = make_result(1);
    r.beat[1] = true;
    publish(r);

    struct audio_telemetry_frame out;
    zassert_true(audio_telemetry_take(&out));
    zassert_equal(out.beat_mask, 1u << 1);

    /* A window that saw no beat must report no beat — a stuck flag would render as a
     * permanently flashing meter. */
    publish(make_result(2));
    zassert_true(audio_telemetry_take(&out));
    zassert_equal(out.beat_mask, 0, "beat flags must clear with the window");
}

ZTEST(audio_telemetry_accum, test_peak_is_the_window_max_not_the_last_frame) {
    setup_active();

    publish(make_result(1), 0.02f);
    publish(make_result(2), 0.90f); /* the loud one, which will be folded away */
    publish(make_result(3), 0.03f);

    struct audio_telemetry_frame out;
    zassert_true(audio_telemetry_take(&out));
    zassert_within(out.peak, 0.90f, 1.0e-6f,
                   "a peak from a dropped frame is still the window's peak");
}

ZTEST(audio_telemetry_accum, test_clip_is_sticky_within_a_window_and_counts_cumulatively) {
    setup_active();

    publish(make_result(1), 0.02f, /* clipped */ false);
    publish(make_result(2), 0.99f, /* clipped */ true);
    publish(make_result(3), 0.02f, /* clipped */ false);

    struct audio_telemetry_frame out;
    zassert_true(audio_telemetry_take(&out));
    zassert_true(out.clipped, "a clip anywhere in the window must be reported");
    zassert_equal(out.clip_count, 1);

    /* The event flag clears with the window; the cumulative count does not. */
    publish(make_result(4));
    zassert_true(audio_telemetry_take(&out));
    zassert_false(out.clipped);
    zassert_equal(out.clip_count, 1, "the running total is the long-run signal");
}

/* ── Staleness reporting ─────────────────────────────────────────────────── */

ZTEST(audio_telemetry_accum, test_take_reports_whether_anything_actually_arrived) {
    setup_active();

    struct audio_telemetry_frame out;
    zassert_false(audio_telemetry_take(&out), "nothing published yet");

    publish(make_result(1));
    zassert_true(audio_telemetry_take(&out));
    zassert_false(audio_telemetry_take(&out), "second take sees no new frame");
}

ZTEST(audio_telemetry_accum, test_dropped_counts_sends_that_carried_nothing_new) {
    setup_active();

    struct audio_telemetry_frame out;
    publish(make_result(1));
    zassert_true(audio_telemetry_take(&out));
    zassert_equal(out.dropped, 0);

    /* Sending anyway is allowed — a frozen meter beats a blank one — but the app has to be
     * able to tell it is looking at a repeat. */
    audio_telemetry_take(&out);
    audio_telemetry_take(&out);
    zassert_equal(out.dropped, 2);
}

/* ── Gating ──────────────────────────────────────────────────────────────── */

ZTEST(audio_telemetry_accum, test_publish_is_a_no_op_while_inactive) {
    audio_telemetry_reset();
    audio_telemetry_set_active(false);

    struct audio_analysis_result r = make_result(1);
    r.beat[0] = true;
    publish(r, 0.5f);

    struct audio_telemetry_frame out;
    zassert_false(audio_telemetry_take(&out), "nothing should have been accumulated");
    zassert_equal(out.beat_mask, 0);
    zassert_equal(out.peak, 0.0f);
}

ZTEST(audio_telemetry_accum, test_active_flag_round_trips) {
    audio_telemetry_set_active(false);
    zassert_false(audio_telemetry_is_active());
    audio_telemetry_set_active(true);
    zassert_true(audio_telemetry_is_active());
    audio_telemetry_set_active(false);
    zassert_false(audio_telemetry_is_active());
}

/* ── Threshold resolution (what the app plots as "fires here") ───────────── */

ZTEST(audio_telemetry_accum, test_mode_0_threshold_is_mean_plus_alpha_sigma) {
    setup_active();

    struct audio_analysis_result r = make_result(1);
    publish(r, 0.01f, false, /* mode */ 0, /* alpha */ 2.0f, /* floor */ 0.0f);

    struct audio_telemetry_frame out;
    zassert_true(audio_telemetry_take(&out));
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        const float want = r.band_mean[b] + 2.0f * r.band_sigma[b];
        zassert_within(out.threshold[b], want, 1.0e-6f, "band %d", b);
    }
}

ZTEST(audio_telemetry_accum, test_mode_1_threshold_is_the_prestored_value) {
    setup_active();

    /* Mode 1 stores the already-resolved threshold in band_sigma — the two slots are
     * deliberately mode-dependent (see audio_dsp.h). Resolving this on-device is what saves
     * every consumer from having to know that. */
    struct audio_analysis_result r = make_result(1);
    publish(r, 0.01f, false, /* mode */ 1, /* alpha */ 2.0f, /* floor */ 0.0f);

    struct audio_telemetry_frame out;
    zassert_true(audio_telemetry_take(&out));
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        zassert_within(out.threshold[b], r.band_sigma[b], 1.0e-6f, "band %d", b);
    }
    zassert_equal(out.threshold_mode, 1, "the mode labels the line, it does not define it");
}

ZTEST(audio_telemetry_accum, test_absolute_floor_clamps_the_resolved_threshold) {
    setup_active();

    /* The detector fires only when flux exceeds BOTH the adaptive threshold and the
     * absolute floor, so the honest "fires here" line is the max of the two. */
    struct audio_analysis_result r = make_result(1);
    publish(r, 0.01f, false, /* mode */ 0, /* alpha */ 0.0f, /* floor */ 5.0f);

    struct audio_telemetry_frame out;
    zassert_true(audio_telemetry_take(&out));
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        zassert_within(out.threshold[b], 5.0f, 1.0e-6f,
                       "floor must dominate when it is above the adaptive line");
    }
}

ZTEST(audio_telemetry_accum, test_raw_stats_are_passed_through_untouched) {
    setup_active();

    /* The calibration wizard replays a recorded window against candidate sensitivities, so
     * it needs the raw statistics — the resolved threshold cannot be inverted back into
     * them, because the floor clamp is lossy. */
    struct audio_analysis_result r = make_result(1);
    publish(r, 0.01f, false, 0, 0.3f, /* a floor that WOULD hide the raw values */ 9.0f);

    struct audio_telemetry_frame out;
    zassert_true(audio_telemetry_take(&out));
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        zassert_within(out.mean[b], r.band_mean[b], 1.0e-6f, "band %d mean", b);
        zassert_within(out.sigma[b], r.band_sigma[b], 1.0e-6f, "band %d sigma", b);
    }
}

/* ── Field plumbing ──────────────────────────────────────────────────────── */

ZTEST(audio_telemetry_accum, test_frames_since_step_saturates_rather_than_wrapping) {
    setup_active();

    publish(make_result(1), 0.01f, false, 0, 0.3f, 0.0f, /* frames_since_step */ 100000);

    struct audio_telemetry_frame out;
    zassert_true(audio_telemetry_take(&out));
    zassert_equal(out.frames_since_step, 255, "a wrap would read as 'just stepped'");
}

ZTEST(audio_telemetry_accum, test_seq_is_truncated_to_16_bits) {
    setup_active();

    publish(make_result(0x12345));
    struct audio_telemetry_frame out;
    zassert_true(audio_telemetry_take(&out));
    zassert_equal(out.seq, 0x2345);
}

ZTEST(audio_telemetry_accum, test_buckets_and_flux_reach_the_frame) {
    setup_active();

    const struct audio_analysis_result r = make_result(9);
    publish(r);

    struct audio_telemetry_frame out;
    zassert_true(audio_telemetry_take(&out));
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) {
        zassert_within(out.flux[b], r.band_flux[b], 1.0e-6f, "flux %d", b);
    }
    for (int i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) {
        zassert_within(out.buckets[i], r.display_bucket_energy[i], 1.0e-6f, "bucket %d", i);
    }
}

ZTEST(audio_telemetry_accum, test_reset_clears_everything_including_counters) {
    setup_active();

    struct audio_analysis_result r = make_result(1);
    r.beat[0] = true;
    publish(r, 0.9f, true);
    struct audio_telemetry_frame out;
    audio_telemetry_take(&out);
    audio_telemetry_take(&out); /* bump dropped */

    /* reset() runs on stream start so the first frame the app sees is not ancient state
     * from a previous session. */
    audio_telemetry_reset();
    audio_telemetry_set_active(true);
    zassert_false(audio_telemetry_take(&out));
    zassert_equal(out.dropped, 1, "the failed take after reset is itself the first drop");
    zassert_equal(out.clip_count, 0);
    zassert_equal(out.beat_mask, 0);
    zassert_equal(out.peak, 0.0f);
}
