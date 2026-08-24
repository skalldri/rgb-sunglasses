/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Stuart Alldritt
 *
 * Simulator shim for <zephyr/kernel.h>.
 *
 * Exists so that source written against the real Zephyr header compiles
 * unchanged for the wasm target. It carries no declarations of its own:
 * printk/vprintk live in <rgbx/rgbx_sys.h>, which is the header extension
 * authors should include directly, and which the simulator's implementations
 * in fw/sim/shim/sim_shim.c are compiled against.
 *
 * Single-sourcing them there is the point — this shim and the SDK's ARM one
 * (fw/sdk/arm/shim/include/zephyr/kernel.h) used to each carry their own
 * printk prototype, and they disagreed on the return type: an extension
 * declaring the device-correct `void printk(...)` linked here against an
 * `int`-returning definition and trapped on first call, because wasm calls
 * are typed by full signature. See issue #351.
 *
 * Deliberately minimal beyond that: anything an extension uses outside the
 * sanctioned set would also fail symbol resolution at llext load time on the
 * device, and the sim should mirror that pressure rather than paper over it.
 */

#ifndef RGBX_SIM_SHIM_ZEPHYR_KERNEL_H_
#define RGBX_SIM_SHIM_ZEPHYR_KERNEL_H_

/* <stdint.h> is NOT redundant: the real <zephyr/kernel.h> provides the
 * fixed-width types transitively, so a TU that includes only this header and
 * then writes `uint8_t` must keep compiling. rgbx_sys.h pulls in stddef/
 * stdarg/string/math but deliberately not stdint — its remit is the allowed
 * FUNCTION surface, not the type surface this shim is standing in for. */
#include <stdint.h>

#include <rgbx/rgbx_sys.h>

#endif /* RGBX_SIM_SHIM_ZEPHYR_KERNEL_H_ */
