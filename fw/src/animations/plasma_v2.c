/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Stuart Alldritt
 *
 * RGBX v2 port of rgbx-plasma at registry revision
 * 02e8c1c9540219d9733ad56e7dd64dcdbae4f180. Q15 recurrence preserves the
 * original three-wave formula without enabling Wasm float opcodes, linking
 * libm, or adding linear memory to the firmware guest.
 * Full MIT permission notice: plasma_v2.LICENSE.
 */
#include <stdint.h>

__attribute__((import_module("rgbx_v2"), import_name("param_u32"))) extern uint32_t rgbx_param_u32(
    uint32_t id);
__attribute__((import_module("rgbx_v2"), import_name("set_luma_span8"))) extern void
rgbx_set_luma_span8(uint32_t first_pixel, uint32_t foreground, uint32_t background, uint32_t luma0,
                    uint32_t luma1, uint32_t luma2, uint32_t luma3, uint32_t luma4, uint32_t luma5,
                    uint32_t luma6, uint32_t luma7);

static __attribute__((address_space(1))) uint32_t time_ms;

static __attribute__((noinline)) uint32_t current_time_ms(void) {
    return time_ms;
}

static inline __attribute__((always_inline)) int32_t round_shift(int64_t value, uint32_t shift) {
    const int64_t half = (int64_t)1 << (shift - 1u);
    return value >= 0 ? (int32_t)((value + half) >> shift) : -(int32_t)((-value + half) >> shift);
}

static __attribute__((noinline)) int32_t bounded_sin_q15(int32_t value_q20) {
    const int32_t pi_q20 = 3294199;
    const int32_t half_pi_q20 = 1647099;
    const int32_t tau_q20 = 6588397;
    int32_t x = value_q20 % tau_q20;
    if (x > pi_q20) {
        x -= tau_q20;
    } else if (x < -pi_q20) {
        x += tau_q20;
    }
    if (x > half_pi_q20) {
        x = pi_q20 - x;
    } else if (x < -half_pi_q20) {
        x = -pi_q20 - x;
    }

    const int32_t x2_q20 = round_shift((int64_t)x * x, 20);
    int32_t polynomial_q30 = 2959;
    polynomial_q30 = -213044 + round_shift((int64_t)x2_q20 * polynomial_q30, 20);
    polynomial_q30 = 8947849 + round_shift((int64_t)x2_q20 * polynomial_q30, 20);
    polynomial_q30 = -178956971 + round_shift((int64_t)x2_q20 * polynomial_q30, 20);
    const int32_t factor_q30 = 1073741824 + round_shift((int64_t)x2_q20 * polynomial_q30, 20);
    const int32_t sine_q20 = round_shift((int64_t)x * factor_q30, 30);
    return round_shift(sine_q20, 5);
}

static inline __attribute__((always_inline)) int32_t rotate_sin_q15(int32_t sine, int32_t cosine,
                                                                    int32_t step_sine,
                                                                    int32_t step_cosine) {
    const int32_t value = sine * step_cosine + cosine * step_sine;
    return (value + 16384 - (value < 0)) >> 15;
}

static inline __attribute__((always_inline)) int32_t advance_sin_q15(int32_t sine,
                                                                     int32_t previous_sine,
                                                                     int32_t doubled_step_cosine) {
    const int32_t value = sine * doubled_step_cosine;
    return ((value + 16384 - (value < 0)) >> 15) - previous_sine;
}

static inline __attribute__((always_inline)) uint32_t plasma_luma(int32_t wave_q15,
                                                                  uint32_t invert) {
    const int32_t three_q15 = 3 * 32768;
    const int32_t value_q15 = invert ? three_q15 - wave_q15 : wave_q15 + three_q15;
    const int32_t bounded_value_q15 =
        value_q15 < 0 ? 0 : (value_q15 > 6 * 32768 ? 6 * 32768 : value_q15);
    return (uint32_t)(bounded_value_q15 * 85) >> 16u;
}

__attribute__((export_name("rgbx_init"))) void rgbx_init(void) {
    time_ms = 0;
}

__attribute__((export_name("rgbx_tick"))) void rgbx_tick(uint32_t dt_ms) {
    const uint32_t period_ms = 62832u;
    const uint32_t speed = rgbx_param_u32(0);
    const uint32_t step = (uint32_t)(((uint64_t)dt_ms * speed / 50u) % period_ms);
    time_ms = (time_ms + step) % period_ms;

    const uint32_t foreground = rgbx_param_u32(1);
    const uint32_t invert = rgbx_param_u32(2) != 0;
    const uint32_t background = rgbx_param_u32(3);

    const uint32_t current_time = current_time_ms();
    const int32_t t1_q20 = (int32_t)(((uint64_t)current_time * 11u * 1048576u) / 10000u);
    const int32_t t2_q20 = (int32_t)(((uint64_t)current_time * 7u * 1048576u) / 10000u);
    const int32_t t3_q20 = (int32_t)(((uint64_t)current_time * 17u * 1048576u) / 10000u);
    const int32_t half_pi_q20 = 1647099;
    const int32_t x_start_sin = bounded_sin_q15(t1_q20);
    const int32_t x_start_cos = bounded_sin_q15(t1_q20 + half_pi_q20);
    const int32_t x_start_previous = rotate_sin_q15(x_start_sin, x_start_cos, -7650, 31863);
    int32_t y_sin = bounded_sin_q15(t2_q20);
    int32_t y_cos = bounded_sin_q15(t2_q20 + half_pi_q20);
    int32_t radial_row_sin = bounded_sin_q15(t3_q20);
    int32_t radial_row_cos = bounded_sin_q15(t3_q20 + half_pi_q20);

    uint32_t first_pixel = 0;
    for (uint32_t y = 0; y < 12u; ++y) {
        int32_t x_sin = x_start_sin;
        int32_t x_previous = x_start_previous;
        int32_t radial_sin = radial_row_sin;
        int32_t radial_previous = rotate_sin_q15(radial_row_sin, radial_row_cos, -5126, 32365);
        for (uint32_t x = 0; x < 40u; x += 8u) {
#define NEXT_PLASMA_LUMA(destination)                                                        \
    do {                                                                                     \
        (destination) = plasma_luma(x_sin + y_sin + radial_sin, invert);                     \
        const int32_t next_x_sin = advance_sin_q15(x_sin, x_previous, 63726);                \
        x_previous = x_sin;                                                                  \
        x_sin = next_x_sin;                                                                  \
        const int32_t next_radial_sin = advance_sin_q15(radial_sin, radial_previous, 64730); \
        radial_previous = radial_sin;                                                        \
        radial_sin = next_radial_sin;                                                        \
    } while (0)
            uint32_t luma0;
            uint32_t luma1;
            uint32_t luma2;
            uint32_t luma3;
            uint32_t luma4;
            uint32_t luma5;
            uint32_t luma6;
            uint32_t luma7;
            NEXT_PLASMA_LUMA(luma0);
            NEXT_PLASMA_LUMA(luma1);
            NEXT_PLASMA_LUMA(luma2);
            NEXT_PLASMA_LUMA(luma3);
            NEXT_PLASMA_LUMA(luma4);
            NEXT_PLASMA_LUMA(luma5);
            NEXT_PLASMA_LUMA(luma6);
            NEXT_PLASMA_LUMA(luma7);
            rgbx_set_luma_span8(first_pixel, foreground, background, luma0, luma1, luma2, luma3,
                                luma4, luma5, luma6, luma7);
#undef NEXT_PLASMA_LUMA
            first_pixel += 8u;
        }
        const int32_t next_y_sin = rotate_sin_q15(y_sin, y_cos, 16384, 28378);
        y_cos = rotate_sin_q15(y_cos, -y_sin, 16384, 28378);
        y_sin = next_y_sin;
        const int32_t next_radial_row_sin =
            rotate_sin_q15(radial_row_sin, radial_row_cos, 16384, 28378);
        radial_row_cos = rotate_sin_q15(radial_row_cos, -radial_row_sin, 16384, 28378);
        radial_row_sin = next_radial_row_sin;
    }
}
