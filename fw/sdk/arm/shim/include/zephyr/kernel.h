/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Stuart Alldritt
 *
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

/* <stdint.h> is NOT redundant: the real <zephyr/kernel.h> provides the
 * fixed-width types transitively, so a TU that includes only this header and
 * then writes `uint8_t` must keep compiling. rgbx_sys.h pulls in stddef/
 * stdarg/string/math but deliberately not stdint — its remit is the allowed
 * FUNCTION surface, not the type surface this shim is standing in for. Kept
 * identical to the simulator's shim so the two targets can't diverge. */
#include <stdint.h>

#include <rgbx/rgbx_sys.h>

#endif /* RGBX_SDK_ZEPHYR_KERNEL_H */
