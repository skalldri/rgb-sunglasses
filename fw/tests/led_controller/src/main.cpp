/*
 * Tests for fw/src/led_controller.cpp, compiled against the fake led_strip
 * driver in this suite (see fake_led_strip.c) so the real display thread runs
 * on native_sim and every led_strip_update_rgb() it makes is recorded.
 *
 * Covers the issue #172 `led_output on|blank|off` panel-output modes (driven
 * through the real shell command via the dummy backend) plus the
 * set_pixel_in_framebuffer geometry contract.
 */

#include <led_controller.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include <string.h>

#include "fake_led_strip.h"
#include "led_stats_core.h"

static const struct device *const strip0 = DEVICE_DT_GET(DT_ALIAS(led_strip_0));
static const struct device *const strip1 = DEVICE_DT_GET(DT_ALIAS(led_strip_1));

// Display thread runs at 10 ms (stub core_config.h); waiting this long always
// spans several display ticks even with native_sim's coarse 10 ms clock.
constexpr int kSeveralTicksMs = 300;

static void run_led_output_cmd(const char *cmd) {
    const struct shell *sh = shell_backend_dummy_get_ptr();

    zassert_not_null(sh, "dummy shell backend missing");
    int ret = shell_execute_cmd(sh, cmd);
    zassert_equal(ret, 0, "'%s' failed: %d", cmd, ret);

    // Let any display tick already in flight under the previous mode finish
    // before tests snapshot the fake strips.
    k_msleep(50);
}

// Render one bright pixel at (0,0) — bank 0, so it lands in strip0's buffer —
// and give the display thread time to adopt the new frame.
static void render_test_pixel(void) {
    size_t buffer = 0;

    zassert_equal(claimBufferForRender(buffer), 0, "claimBufferForRender failed");
    zassert_equal(set_pixel_in_framebuffer(get_current_led_config(), 0, 0, buffer, 255, 128, 64),
                  0, "set_pixel_in_framebuffer failed");
    zassert_equal(releaseBufferFromRender(buffer), 0, "releaseBufferFromRender failed");
    k_msleep(50);
}

static bool last_frame_all_black(const struct device *dev) {
    size_t len = 0;
    const struct led_rgb *frame = fake_led_strip_last_frame(dev, &len);

    for (size_t i = 0; i < len; i++) {
        if (frame[i].r != 0 || frame[i].g != 0 || frame[i].b != 0) {
            return false;
        }
    }
    return true;
}

static bool last_frame_has_lit_pixel(const struct device *dev) {
    size_t len = 0;
    (void)fake_led_strip_last_frame(dev, &len);

    return len > 0 && !last_frame_all_black(dev);
}

ZTEST(led_controller, test_normal_mode_clocks_rendered_frames) {
    run_led_output_cmd("led_output on");
    render_test_pixel();

    fake_led_strip_reset(strip0);
    fake_led_strip_reset(strip1);
    k_msleep(kSeveralTicksMs);

    zassert_true(fake_led_strip_update_count(strip0) > 0, "strip0 not clocked in normal mode");
    zassert_true(fake_led_strip_update_count(strip1) > 0, "strip1 not clocked in normal mode");
    zassert_true(last_frame_has_lit_pixel(strip0),
                 "rendered pixel missing from strip0 in normal mode");
}

ZTEST(led_controller, test_off_stops_clocking_entirely) {
    run_led_output_cmd("led_output on");
    render_test_pixel();
    run_led_output_cmd("led_output off");

    uint32_t count0 = fake_led_strip_update_count(strip0);
    uint32_t count1 = fake_led_strip_update_count(strip1);

    k_msleep(kSeveralTicksMs);

    zassert_equal(fake_led_strip_update_count(strip0), count0,
                  "strip0 still clocked in off mode");
    zassert_equal(fake_led_strip_update_count(strip1), count1,
                  "strip1 still clocked in off mode");

    run_led_output_cmd("led_output on");
}

ZTEST(led_controller, test_blank_mode_clocks_all_black_frames) {
    run_led_output_cmd("led_output on");
    render_test_pixel();
    run_led_output_cmd("led_output blank");

    fake_led_strip_reset(strip0);
    fake_led_strip_reset(strip1);
    k_msleep(kSeveralTicksMs);

    // Still clocking every tick, but all-black data despite the lit framebuffer
    zassert_true(fake_led_strip_update_count(strip0) > 0, "strip0 not clocked in blank mode");
    zassert_true(fake_led_strip_update_count(strip1) > 0, "strip1 not clocked in blank mode");
    zassert_true(last_frame_all_black(strip0), "strip0 frame not black in blank mode");
    zassert_true(last_frame_all_black(strip1), "strip1 frame not black in blank mode");

    run_led_output_cmd("led_output on");
}

ZTEST(led_controller, test_on_restores_rendered_output_after_blank) {
    run_led_output_cmd("led_output on");
    render_test_pixel();
    run_led_output_cmd("led_output blank");
    k_msleep(kSeveralTicksMs);
    run_led_output_cmd("led_output on");
    k_msleep(kSeveralTicksMs);

    // The previously rendered frame (still the last-released render buffer)
    // must come back without a new render pass.
    zassert_true(last_frame_has_lit_pixel(strip0),
                 "rendered pixel did not return after blank -> on");
}

ZTEST(led_controller, test_set_pixel_bounds_and_population) {
    size_t buffer = 0;

    zassert_equal(claimBufferForRender(buffer), 0, "claimBufferForRender failed");

    const LedConfig *config = get_current_led_config();

    // Off the display edge -> -1
    zassert_equal(set_pixel_in_framebuffer(config, 40, 0, buffer, 255, 0, 0), -1,
                  "x=40 should be off-panel");
    zassert_equal(set_pixel_in_framebuffer(config, 0, 12, buffer, 255, 0, 0), -1,
                  "y=12 should be off-panel");

    // Inside the nose cutout (row 9 has only 16 LEDs per bank) -> -2
    zassert_equal(set_pixel_in_framebuffer(config, 16, 9, buffer, 255, 0, 0), -2,
                  "(16,9) should be unpopulated");

    // Bottom-right corner is populated
    zassert_equal(set_pixel_in_framebuffer(config, 39, 11, buffer, 255, 0, 0), 0,
                  "(39,11) should be populated");

    zassert_equal(releaseBufferFromRender(buffer), 0, "releaseBufferFromRender failed");
}


/* ------------------------------------------------------------------------
 * Frame-pacing stats (issue #267): led_stats_core logic + the shell command.
 * ------------------------------------------------------------------------ */

ZTEST(led_controller, test_stats_reset_clears_and_seeds_min) {
    led_stats_core::Stats s{};
    led_stats_core::recordFrame(s, true, 40000, 5000, 1000, 33300);
    led_stats_core::recordOverrun(s);

    led_stats_core::reset(s);

    zassert_equal(s.frames, 0, "reset should clear the frame count");
    zassert_equal(s.overruns, 0, "reset should clear overruns");
    zassert_equal(s.intervalMaxUs, 0, "reset should clear max");
    zassert_equal(s.intervalSumUs, 0, "reset should clear the sum");
    // Seeded high so the first real sample always wins the min comparison.
    zassert_equal(s.intervalMinUs, UINT32_MAX, "reset should seed min with the sentinel");
}

ZTEST(led_controller, test_stats_first_frame_records_no_interval) {
    led_stats_core::Stats s;
    led_stats_core::reset(s);

    // haveInterval=false: there is no previous wake-up to measure against. The work and
    // segment figures still count — only the interval is skipped.
    led_stats_core::recordFrame(s, false, 999999, 7000, 2000, 33300);

    zassert_equal(s.frames, 1, "the frame itself should still count");
    zassert_equal(s.workMaxUs, 7000, "work should be recorded on the first frame");
    zassert_equal(s.worstSegmentUs, 2000, "segment should be recorded on the first frame");
    zassert_equal(s.intervalSumUs, 0, "no interval should be accumulated");
    zassert_equal(s.intervalMaxUs, 0, "no interval should be recorded");

    // ...and it must not leak the sentinel into the report.
    led_stats_core::Summary sum = led_stats_core::summarize(s);
    zassert_equal(sum.intervalSamples, 0, "one frame means zero interval samples");
    zassert_equal(sum.intervalMinUs, 0, "min must not report the UINT32_MAX sentinel");
    zassert_equal(sum.intervalAvgUs, 0, "avg must not divide by zero");
}

ZTEST(led_controller, test_stats_tracks_min_max_and_average) {
    led_stats_core::Stats s;
    led_stats_core::reset(s);

    led_stats_core::recordFrame(s, false, 0, 1000, 500, 33300);  // seed frame
    led_stats_core::recordFrame(s, true, 30000, 1000, 500, 33300);
    led_stats_core::recordFrame(s, true, 40000, 1000, 500, 33300);
    led_stats_core::recordFrame(s, true, 35000, 1000, 500, 33300);

    led_stats_core::Summary sum = led_stats_core::summarize(s);
    zassert_equal(sum.frames, 4, "four frames recorded");
    zassert_equal(sum.intervalSamples, 3, "intervals are recorded from the 2nd frame on");
    zassert_equal(sum.intervalMinUs, 30000, "min interval");
    zassert_equal(sum.intervalMaxUs, 40000, "max interval");
    zassert_equal(sum.intervalAvgUs, 35000, "avg of 30000/40000/35000");
}

ZTEST(led_controller, test_stats_late_frame_threshold) {
    led_stats_core::Stats s;
    led_stats_core::reset(s);
    const uint32_t target = 33300;

    led_stats_core::recordFrame(s, false, 0, 0, 0, target);
    // Exactly 2x is NOT late — the comparison is strictly greater.
    led_stats_core::recordFrame(s, true, 2 * target, 0, 0, target);
    zassert_equal(s.lateFrames, 0, "exactly 2x target must not count as late");

    led_stats_core::recordFrame(s, true, 2 * target + 1, 0, 0, target);
    zassert_equal(s.lateFrames, 1, "just over 2x target is late");

    // A zero target (display rate not yet readable) must not classify everything as late.
    led_stats_core::recordFrame(s, true, 500000, 0, 0, 0);
    zassert_equal(s.lateFrames, 1, "a zero target must not produce late frames");
}

ZTEST(led_controller, test_stats_keeps_worst_not_last) {
    led_stats_core::Stats s;
    led_stats_core::reset(s);

    led_stats_core::recordFrame(s, false, 0, 9000, 8000, 33300);
    led_stats_core::recordFrame(s, true, 33000, 100, 100, 33300);  // a fast frame after

    zassert_equal(s.workMaxUs, 9000, "work max must survive a later smaller sample");
    zassert_equal(s.worstSegmentUs, 8000, "worst segment must survive a later smaller sample");
}

ZTEST(led_controller, test_led_stats_shell_command) {
    const struct shell *sh = shell_backend_dummy_get_ptr();
    zassert_not_null(sh, "dummy shell backend missing");

    // The real display thread is running in this suite, so frames accumulate on their own.
    zassert_equal(shell_execute_cmd(sh, "led_stats reset"), 0, "led_stats reset failed");
    k_msleep(kSeveralTicksMs);

    shell_backend_dummy_clear_output(sh);
    zassert_equal(shell_execute_cmd(sh, "led_stats"), 0, "led_stats failed");

    size_t len = 0;
    const char *out = shell_backend_dummy_get_output(sh, &len);
    zassert_not_null(out, "no shell output captured");
    zassert_not_null(strstr(out, "interval:"), "led_stats should report the interval line");
    zassert_not_null(strstr(out, "worst segment:"), "led_stats should report worst segment");
}

ZTEST_SUITE(led_controller, NULL, NULL, NULL, NULL, NULL);
