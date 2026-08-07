/*
 * Simulator shim for <zephyr/kernel.h>.
 *
 * Extensions may include this for printk() (the only Zephyr call the in-repo
 * extensions make — the rgbx ABI itself has zero imports). The simulator
 * implements printk in fw/sim/shim/sim_shim.c, formatting into an exported
 * log buffer the harness drains after each tick.
 *
 * Deliberately minimal: anything an extension uses beyond this list would
 * also fail symbol resolution at llext load time on the device, and the sim
 * should mirror that pressure rather than paper over it.
 */

#ifndef RGBX_SIM_SHIM_ZEPHYR_KERNEL_H_
#define RGBX_SIM_SHIM_ZEPHYR_KERNEL_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int printk(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* RGBX_SIM_SHIM_ZEPHYR_KERNEL_H_ */
