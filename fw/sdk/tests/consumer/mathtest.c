/* SDK-gate test extension: exercises the issue #295 export surface — the
 * curated single-precision libm set, 64-bit integer division
 * (__aeabi_uldivmod on ARM), and memmove. Exists so sdk-ci proves the
 * undefined-symbol gate ACCEPTS these (and so its nm assertion can prove
 * the calls weren't inlined away). Not an interesting animation.
 *
 * Per-tick CPU cost (12 transcendental calls x 40 columns) is
 * UNMEASURED-BY-DESIGN: this is a CI link-gate fixture under fw/sdk/tests/,
 * never shipped and never installed by /provision-device. Measuring the
 * real on-device cost of transcendentals in a tick loop (`ext stats`) is
 * part of issue #295's deferred hardware checklist — do that measurement
 * before pointing at THIS file as budget evidence for anything.
 */

/* <rgbx/rgbx_sys.h> is the sanctioned way to reach printk and the libm/string
 * subset (issue #351) — it pulls in <math.h>/<string.h> and declares
 * printk/vprintk itself. Included here rather than the individual headers so
 * sdk-ci compiles it under BOTH real extension toolchains, not just the host
 * compiler that check-sys-header.sh defaults to. */
#include <rgbx/rgbx_api.h>
#include <rgbx/rgbx_sys.h>
#include <zephyr/llext/symbol.h>

#define WIDTH 40u
#define HEIGHT 12u

struct rgbx_inputs rgbx_inputs;
uint8_t rgbx_framebuffer[WIDTH * HEIGHT * 3u];

const struct rgbx_manifest rgbx_manifest = {
    .abi_version = RGBX_ABI_VERSION,
    .name = "Math Test",
    .width = WIDTH,
    .height = HEIGHT,
    .param_count = 0,
    .params = 0,
};

static uint64_t ticks;

void rgbx_init(void)
{
	ticks = 0;
	printk("mathtest: libm surface check\n");
}

void rgbx_tick(void)
{
	ticks += rgbx_inputs.dt_ms;
	/* Volatile divisor defeats strength reduction so the compiler must
	 * emit the __aeabi_uldivmod call this test exists to exercise. The
	 * variable-count 64-bit shift is exercised too, but NOT nm-asserted:
	 * GCC at the SDK's pinned -O2 inlines it (lsl/orr sequence) instead
	 * of calling __aeabi_llsl — the shift helpers are exported
	 * defensively for codegen that does emit them (-Os, other
	 * compilers), which is exactly why they can't be a reliable
	 * assertion target here.
	 */
	volatile uint64_t div = 7u + (rgbx_inputs.buttons_pressed & 1u);
	volatile unsigned shift = 3u + (rgbx_inputs.buttons_pressed & 1u);
	const uint64_t phase = (ticks << shift) / div;

	const float t = (float)phase * 0.02f;
	for (uint32_t x = 0; x < WIDTH; x++) {
		const float fx = (float)x / (float)WIDTH;
		const float v = sinf(t + fx * 6.28318f) * cosf(t * 0.5f)
			+ atan2f(fx, 1.0f) + sqrtf(fx)
			+ expf(-fx) + logf(1.0f + fx) + powf(fx, 1.5f)
			+ fmodf(t, 1.0f) + floorf(fx * 4.0f) + ceilf(fx * 2.0f)
			+ roundf(fx * 8.0f) + tanf(fx * 0.5f);
		const uint32_t y = (uint32_t)v % HEIGHT;
		rgbx_framebuffer[RGBX_PIXEL_INDEX(WIDTH, x, y)] = 255u;
	}
	/* memmove: scroll the framebuffer one pixel (overlapping ranges). */
	memmove(rgbx_framebuffer, rgbx_framebuffer + 3, sizeof(rgbx_framebuffer) - 3);
}

EXPORT_SYMBOL(rgbx_manifest);
EXPORT_SYMBOL(rgbx_inputs);
EXPORT_SYMBOL(rgbx_framebuffer);
EXPORT_SYMBOL(rgbx_init);
EXPORT_SYMBOL(rgbx_tick);
