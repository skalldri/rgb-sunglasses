/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Minimal conforming RGBX v2 guest, built through the SDK's public contract.
 *
 * It exists to exercise the release path end to end rather than to look good
 * on the panel: it reads one numeric parameter and one capability-gated
 * input, paints a complete frame through the RGB span encoding, and stops.
 * Requesting the buttons capability is deliberate: a package that asks for
 * no permission at all cannot show that the device refuses one it was never
 * granted.
 */
#include <rgbx/rgbx_v2.h>

RGBX_V2_EXPORT("rgbx_init") void rgbx_init(void) {}

RGBX_V2_EXPORT("rgbx_tick") void rgbx_tick(uint32_t dt_ms) {
    const uint32_t buttons = rgbx_v2_input_u32(RGBX_V2_INPUT_BUTTONS_PRESSED, 0);
    const uint32_t color = (rgbx_v2_param_u32(0) ^ dt_ms ^ buttons) & 0x00ffffffu;
    for (uint32_t first = 0; first < RGBX_V2_PIXEL_COUNT; first += RGBX_V2_PIXELS_PER_SPAN) {
        rgbx_v2_set_span8(first, color, color, color, color, color, color, color, color);
    }
}
