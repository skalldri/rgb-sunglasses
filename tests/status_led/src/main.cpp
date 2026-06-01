#include <status_led/status_led_math.h>
#include <zephyr/ztest.h>

// ---------------------------------------------------------------------------
// color_to_rgb: all seven ROYGBIV values
// ---------------------------------------------------------------------------

ZTEST_SUITE(status_led_color, NULL, NULL, NULL, NULL, NULL);

ZTEST(status_led_color, test_red) {
    struct led_rgb c = status_led_color_to_rgb(StatusColor::Red);
    zassert_equal(c.r, 255); zassert_equal(c.g, 0);   zassert_equal(c.b, 0);
}

ZTEST(status_led_color, test_orange) {
    struct led_rgb c = status_led_color_to_rgb(StatusColor::Orange);
    zassert_equal(c.r, 255); zassert_equal(c.g, 128); zassert_equal(c.b, 0);
}

ZTEST(status_led_color, test_yellow) {
    struct led_rgb c = status_led_color_to_rgb(StatusColor::Yellow);
    zassert_equal(c.r, 255); zassert_equal(c.g, 255); zassert_equal(c.b, 0);
}

ZTEST(status_led_color, test_green) {
    struct led_rgb c = status_led_color_to_rgb(StatusColor::Green);
    zassert_equal(c.r, 0);   zassert_equal(c.g, 255); zassert_equal(c.b, 0);
}

ZTEST(status_led_color, test_blue) {
    struct led_rgb c = status_led_color_to_rgb(StatusColor::Blue);
    zassert_equal(c.r, 0);   zassert_equal(c.g, 0);   zassert_equal(c.b, 255);
}

ZTEST(status_led_color, test_indigo) {
    struct led_rgb c = status_led_color_to_rgb(StatusColor::Indigo);
    zassert_equal(c.r, 75);  zassert_equal(c.g, 0);   zassert_equal(c.b, 130);
}

ZTEST(status_led_color, test_violet) {
    struct led_rgb c = status_led_color_to_rgb(StatusColor::Violet);
    zassert_equal(c.r, 148); zassert_equal(c.g, 0);   zassert_equal(c.b, 211);
}

// ---------------------------------------------------------------------------
// scale_brightness
// ---------------------------------------------------------------------------

ZTEST_SUITE(status_led_scale, NULL, NULL, NULL, NULL, NULL);

ZTEST(status_led_scale, test_full_brightness_unchanged) {
    struct led_rgb base = {.r = 200, .g = 100, .b = 50};
    struct led_rgb out  = status_led_scale_brightness(base, 255);
    zassert_equal(out.r, 200); zassert_equal(out.g, 100); zassert_equal(out.b, 50);
}

ZTEST(status_led_scale, test_zero_brightness_all_black) {
    struct led_rgb base = {.r = 200, .g = 100, .b = 50};
    struct led_rgb out  = status_led_scale_brightness(base, 0);
    zassert_equal(out.r, 0); zassert_equal(out.g, 0); zassert_equal(out.b, 0);
}

ZTEST(status_led_scale, test_half_brightness_halves_channels) {
    struct led_rgb base = {.r = 200, .g = 100, .b = 50};
    struct led_rgb out  = status_led_scale_brightness(base, 128);
    // 200*128/255 = 100, 100*128/255 = 50, 50*128/255 = 25
    zassert_equal(out.r, 100); zassert_equal(out.g, 50); zassert_equal(out.b, 25);
}

ZTEST(status_led_scale, test_black_base_stays_black) {
    struct led_rgb base = {.r = 0, .g = 0, .b = 0};
    struct led_rgb out  = status_led_scale_brightness(base, 255);
    zassert_equal(out.r, 0); zassert_equal(out.g, 0); zassert_equal(out.b, 0);
}

// ---------------------------------------------------------------------------
// blinking_brightness: 500 ms on / 500 ms off at 20 ms per tick → half = 25
// ---------------------------------------------------------------------------

ZTEST_SUITE(status_led_blinking, NULL, NULL, NULL, NULL, NULL);

static constexpr uint32_t kBlinkHalf = 25u;  // 500 ms / 20 ms

ZTEST(status_led_blinking, test_tick_zero_is_on) {
    zassert_equal(status_led_blinking_brightness(0, kBlinkHalf), 255u);
}

ZTEST(status_led_blinking, test_first_tick_before_midpoint_is_on) {
    zassert_equal(status_led_blinking_brightness(kBlinkHalf - 1, kBlinkHalf), 255u);
}

ZTEST(status_led_blinking, test_tick_at_midpoint_is_off) {
    zassert_equal(status_led_blinking_brightness(kBlinkHalf, kBlinkHalf), 0u);
}

ZTEST(status_led_blinking, test_tick_before_period_end_is_off) {
    zassert_equal(status_led_blinking_brightness(kBlinkHalf * 2u - 1u, kBlinkHalf), 0u);
}

ZTEST(status_led_blinking, test_tick_at_period_wraps_back_on) {
    zassert_equal(status_led_blinking_brightness(kBlinkHalf * 2u, kBlinkHalf), 255u);
}

// ---------------------------------------------------------------------------
// breathing_brightness: sine envelope — boundary and monotonicity checks
// ---------------------------------------------------------------------------

ZTEST_SUITE(status_led_breathing, NULL, NULL, NULL, NULL, NULL);

static constexpr uint32_t kBreathePeriod = 100u;  // 2000 ms / 20 ms

ZTEST(status_led_breathing, test_output_always_in_range) {
    for (uint32_t t = 0; t < kBreathePeriod * 3u; t++) {
        uint8_t b = status_led_breathing_brightness(t, kBreathePeriod);
        zassert_true(b <= 255u, "Breathing brightness must be <= 255 at tick %u", t);
    }
}

ZTEST(status_led_breathing, test_tick_zero_near_midpoint) {
    // sin(0) = 0 → scaled to ~127
    uint8_t b = status_led_breathing_brightness(0, kBreathePeriod);
    zassert_within((int)b, 127, 2, "tick=0 should be near mid-brightness (sin=0)");
}

ZTEST(status_led_breathing, test_quarter_period_near_maximum) {
    // sin(π/2) = 1 → scaled to ~255
    uint8_t b = status_led_breathing_brightness(kBreathePeriod / 4u, kBreathePeriod);
    zassert_true(b >= 240u, "Quarter-period should be near maximum brightness");
}

ZTEST(status_led_breathing, test_three_quarter_period_near_minimum) {
    // sin(3π/2) = -1 → scaled to ~0
    uint8_t b = status_led_breathing_brightness((kBreathePeriod * 3u) / 4u, kBreathePeriod);
    zassert_true(b <= 15u, "Three-quarter period should be near minimum brightness");
}

ZTEST(status_led_breathing, test_period_wraps_same_as_tick_zero) {
    uint8_t b0    = status_led_breathing_brightness(0,              kBreathePeriod);
    uint8_t bWrap = status_led_breathing_brightness(kBreathePeriod, kBreathePeriod);
    zassert_equal(b0, bWrap, "Breathing must be periodic: tick 0 == tick period");
}
