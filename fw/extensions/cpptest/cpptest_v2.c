/*
 * RGBX v2 prototype port of cpptest.cpp.
 *
 * The phone compiler's memoryless profile lowers private scalar state to Wasm
 * numeric globals and inlines helpers before admission. The checked test
 * fixture is that lowered form, not an ordinary WASI link with a 64 KiB linear
 * memory declaration.
 */

#include <stdint.h>

__attribute__((import_module("rgbx_v2"), import_name("param_u32"))) extern uint32_t rgbx_param_u32(
    uint32_t id);

__attribute__((import_module("rgbx_v2"), import_name("set_span8"))) extern void rgbx_set_span8(
    uint32_t first_pixel, uint32_t color0, uint32_t color1, uint32_t color2, uint32_t color3,
    uint32_t color4, uint32_t color5, uint32_t color6, uint32_t color7);

static uint32_t time_ms;

static inline uint32_t wave8(uint32_t angle) {
    const uint32_t t = angle & 0xffu;
    const uint32_t half = t & 0x7fu;
    const uint32_t hump = half * (127u - half) / 32u;
    return (t & 0x80u) ? 128u - hump : 128u + hump;
}

static inline uint32_t pixel_color(uint32_t x, uint32_t y, uint32_t color, uint32_t invert) {
    uint32_t value = (wave8(x * 13u + time_ms / 9u) + wave8(y * 23u + time_ms / 14u) +
                      wave8((x + y) * 11u + time_ms / 6u)) /
                     3u;
    if (invert) {
        value = 255u - value;
    }
    const uint32_t red = (color >> 16u) & 0xffu;
    const uint32_t green = (color >> 8u) & 0xffu;
    const uint32_t blue = color & 0xffu;
    return ((red * value / 255u) << 16u) | ((green * value / 255u) << 8u) | (blue * value / 255u);
}

__attribute__((export_name("rgbx_init"))) void rgbx_init(void) {
    time_ms = 0;
}

__attribute__((export_name("rgbx_tick"))) void rgbx_tick(uint32_t dt_ms) {
    time_ms += dt_ms * rgbx_param_u32(0) / 50u;
    const uint32_t color = rgbx_param_u32(1);
    const uint32_t invert = rgbx_param_u32(2) != 0;

    for (uint32_t y = 0; y < 12u; ++y) {
        for (uint32_t x = 0; x < 40u; x += 8u) {
            rgbx_set_span8(
                y * 40u + x, pixel_color(x, y, color, invert),
                pixel_color(x + 1u, y, color, invert), pixel_color(x + 2u, y, color, invert),
                pixel_color(x + 3u, y, color, invert), pixel_color(x + 4u, y, color, invert),
                pixel_color(x + 5u, y, color, invert), pixel_color(x + 6u, y, color, invert),
                pixel_color(x + 7u, y, color, invert));
        }
    }
}
