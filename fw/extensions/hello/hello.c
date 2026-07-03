/*
 * hello — the smallest possible rgbx animation extension, written in plain C
 * against the raw flat ABI (no C++ wrapper) to prove the wire contract
 * independently. Logs from inside the sandbox on init and renders a slow
 * white scanning dot so activation is visible on the frame.
 */

#include <rgbx/rgbx_api.h>
#include <zephyr/kernel.h>
#include <zephyr/llext/symbol.h>

#define WIDTH 40u
#define HEIGHT 12u

struct rgbx_inputs rgbx_inputs;
uint8_t rgbx_framebuffer[WIDTH * HEIGHT * 3u];

const struct rgbx_manifest rgbx_manifest = {
    .abi_version = RGBX_ABI_VERSION,
    .name = "Hello Extension",
    .width = WIDTH,
    .height = HEIGHT,
    .param_count = 0,
    .params = 0,
};

static uint32_t pos_ms;

void rgbx_init(void)
{
	pos_ms = 0;
	printk("hello: rgbx_init running inside the sandbox\n");
}

void rgbx_tick(void)
{
	pos_ms += rgbx_inputs.dt_ms;

	/* One full sweep across the display every ~2 seconds. */
	uint32_t x = (pos_ms / 50u) % WIDTH;

	for (uint32_t i = 0; i < sizeof(rgbx_framebuffer); i++) {
		rgbx_framebuffer[i] = 0;
	}
	for (uint32_t y = 0; y < HEIGHT; y++) {
		uint8_t *px = &rgbx_framebuffer[RGBX_PIXEL_INDEX(WIDTH, x, y)];
		px[0] = 32;
		px[1] = 32;
		px[2] = 32;
	}
}

EXPORT_SYMBOL(rgbx_manifest);
EXPORT_SYMBOL(rgbx_inputs);
EXPORT_SYMBOL(rgbx_framebuffer);
EXPORT_SYMBOL(rgbx_init);
EXPORT_SYMBOL(rgbx_tick);
