/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2023 Intel Corporation
 * Copyright (c) 2026 Stuart Alldritt
 *
 * Derived from the Zephyr Project's include/zephyr/llext/symbol.h, which is
 * licensed Apache-2.0 and carries "Copyright (c) 2023 Intel Corporation".
 * The struct layout and the LL_EXTENSION_BUILD form of EXPORT_SYMBOL below
 * are reproduced from it so an extension built against this SDK emits the
 * bytes the on-device loader reads; the rest of this file is original and
 * the Stuart Alldritt copyright above covers it. The rest of the SDK is
 * MIT-licensed; this one Zephyr-derived file stays Apache-2.0, and the
 * archive's NOTICE records that origin.
 *
 * Standalone-SDK shim for <zephyr/llext/symbol.h> — ARM .llext builds only.
 *
 * Emits the exact on-wire structure the Zephyr llext loader consumes
 * (struct llext_const_symbol entries in section .exported_sym), flattened so
 * no other Zephyr headers are needed. Mirrors the LL_EXTENSION_BUILD branch
 * of NCS v3.1.1's zephyr/include/zephyr/llext/symbol.h; the struct is fixed
 * at two pointer-sized words, and the static_assert below is the contract
 * check against the on-device loader.
 *
 * This is NOT the same file as the simulator's wasm shim of the same name
 * (fw/sim/shim/include/zephyr/llext/symbol.h), where EXPORT_SYMBOL is a
 * no-op: wasm export visibility comes from -Wl,--export-if-defined instead.
 * The SDK ships both, under arm/shim/ and wasm/shim/ respectively, and the
 * per-target toolchain selects which include tree is in scope.
 */

#ifndef RGBX_SDK_ZEPHYR_LLEXT_SYMBOL_H
#define RGBX_SDK_ZEPHYR_LLEXT_SYMBOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct llext_const_symbol {
	/* At build time the loader only reads .name; the union with a
	 * Symbol Link Identifier matches the real header's layout.
	 */
	union {
		const char *const name;
		const uintptr_t slid;
	};

	const void *const addr;
};

#ifdef __cplusplus
static_assert(sizeof(struct llext_const_symbol) == 2 * sizeof(uintptr_t),
	      "llext_const_symbol layout must match the on-device loader");
#else
_Static_assert(sizeof(struct llext_const_symbol) == 2 * sizeof(uintptr_t),
	       "llext_const_symbol layout must match the on-device loader");
#endif

#define RGBX_SDK_STRINGIFY_(x) #x
#define RGBX_SDK_STRINGIFY(x) RGBX_SDK_STRINGIFY_(x)

#ifdef LL_EXTENSION_BUILD
#define EXPORT_SYMBOL(x)						\
	static const struct llext_const_symbol				\
		__attribute__((section(".exported_sym"), used))		\
		__llext_sym_##x = {					\
			.name = RGBX_SDK_STRINGIFY(x),			\
			.addr = (const void *)&x,			\
	}
#else
#define EXPORT_SYMBOL(x)
#endif

#ifdef __cplusplus
}
#endif

#endif /* RGBX_SDK_ZEPHYR_LLEXT_SYMBOL_H */
