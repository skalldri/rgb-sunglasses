#include <render_pacing.h>
#include <zephyr/ztest.h>

using render_pacing::framesPerRender;
using render_pacing::renderIntervalMs;

ZTEST_SUITE(render_pacing_tests, NULL, NULL, NULL, NULL, NULL);

/* Defaults: render == display -> N = 1, dt = the display interval. */
ZTEST(render_pacing_tests, test_defaults_are_one_to_one) {
    zassert_equal(framesPerRender(33.3f, 33.3f), 1, "equal rates must give N=1");
    zassert_within(renderIntervalMs(33.3f, 33.3f), 33.3f, 0.001f, "dt must be the display interval");
}

/* PR #381 review: the (1.0, 1.5) ratio band must round UP — the effective
 * render interval is never SHORTER than requested. round() gave N=1 here,
 * silently rendering 20% more than a 40 ms setting asked for. */
ZTEST(render_pacing_tests, test_sub_half_ratio_rounds_up_never_faster) {
    zassert_equal(framesPerRender(40.0f, 33.3f), 2, "40 ms under a 33.3 ms display must give N=2");
    zassert_within(renderIntervalMs(40.0f, 33.3f), 66.6f, 0.001f,
                   "the effective interval must not be shorter than requested");
}

/* Exact multiples must stay exact despite float representation (the epsilon):
 * 2.0 stored as 1.999... must not gain a spurious frame. */
ZTEST(render_pacing_tests, test_exact_multiples_are_exact) {
    zassert_equal(framesPerRender(66.6f, 33.3f), 2, "exact 2x must give N=2");
    zassert_equal(framesPerRender(99.9f, 33.3f), 3, "exact 3x must give N=3");
    zassert_equal(framesPerRender(100000.0f, 10000.0f), 10, "exact 10x must give N=10");
}

/* Strict ceiling above an exact multiple: 100/33.3 = 3.003 -> N=4 (99.9 ms
 * would be 0.1 ms faster than requested). */
ZTEST(render_pacing_tests, test_just_above_multiple_rounds_up) {
    zassert_equal(framesPerRender(100.0f, 33.3f), 4, "3.003x must ceil to N=4");
}

/* Below-display requests clamp to N=1 (getRenderRateMs()'s floor normally
 * prevents these reaching here; the seam must still never return 0). */
ZTEST(render_pacing_tests, test_faster_than_display_clamps_to_one) {
    zassert_equal(framesPerRender(11.1f, 33.3f), 1, "a below-display request must give N=1");
    zassert_equal(framesPerRender(0.0f, 33.3f), 1, "a zero render rate must give N=1");
}

/* PR #381 review: an unusable display interval (0 is remotely writable and
 * unclamped) must not collapse dt to 0 — the fallback is the render rate, and
 * the divider is meaningless (the caller self-paces on this path). */
ZTEST(render_pacing_tests, test_unusable_display_interval_falls_back) {
    zassert_equal(framesPerRender(33.3f, 0.0f), 1, "display 0 must give N=1");
    zassert_within(renderIntervalMs(33.3f, 0.0f), 33.3f, 0.001f,
                   "display 0 must fall back to the render rate for dt");
    zassert_within(renderIntervalMs(50.0f, -1.0f), 50.0f, 0.001f,
                   "a negative display interval must fall back too");
    // Both rates written to 0 (getRenderRateMs()'s floor is a no-op when the
    // display rate is 0): dt must still never be 0 — animations would freeze
    // and shuffle's dwell clock would stop (PR #381 review).
    zassert_within(renderIntervalMs(0.0f, 0.0f), 1.0f, 0.001f,
                   "dt must be floored at 1 ms even with both rates zero");
}
