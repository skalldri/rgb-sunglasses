/*
 * Standalone-SDK shim for <zephyr/kernel.h> — ARM .llext builds only.
 *
 * Exists so that source written against the real Zephyr header (the in-repo
 * extensions, and anything copied from them) compiles unchanged in a
 * standalone SDK tree. It carries no declarations of its own: printk/vprintk
 * live in <rgbx/rgbx_sys.h>, which is the header extension authors should
 * include directly.
 *
 * Single-sourcing them there is the point — this shim and the simulator's
 * (fw/sim/shim/include/zephyr/kernel.h) used to each carry their own printk
 * prototype, and they disagreed on the return type. See issue #351.
 */

#ifndef RGBX_SDK_ZEPHYR_KERNEL_H
#define RGBX_SDK_ZEPHYR_KERNEL_H

#include <rgbx/rgbx_sys.h>

#endif /* RGBX_SDK_ZEPHYR_KERNEL_H */
