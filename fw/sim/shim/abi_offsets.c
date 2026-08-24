/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Stuart Alldritt
 *
 * Compile-time proof that the wasm32 layout of the rgbx ABI structs matches
 * the device's ARM EABI layout (both are ILP32 with natural alignment).
 *
 * Compiled into every extension .wasm build. The same constants are
 * duplicated in fw/sim/core/abi.ts, which the harness uses to read/write
 * extension linear memory — if either side drifts from rgbx_api.h, this file
 * fails the build (here) or the abi unit test fails (TS side).
 */

#include <rgbx/rgbx_api.h>
#include <stddef.h>

_Static_assert(offsetof(struct rgbx_inputs, dt_ms) == 0, "abi drift: dt_ms");
_Static_assert(offsetof(struct rgbx_inputs, params) == 4, "abi drift: params");
_Static_assert(offsetof(struct rgbx_inputs, param_strings) == 68, "abi drift: param_strings");
_Static_assert(offsetof(struct rgbx_inputs, accel) == 196, "abi drift: accel");
_Static_assert(offsetof(struct rgbx_inputs, gyro) == 208, "abi drift: gyro");
_Static_assert(offsetof(struct rgbx_inputs, audio_band_energy) == 220,
               "abi drift: audio_band_energy");
_Static_assert(offsetof(struct rgbx_inputs, audio_beat) == 236, "abi drift: audio_beat");
_Static_assert(offsetof(struct rgbx_inputs, audio_display_bucket) == 240,
               "abi drift: audio_display_bucket");
_Static_assert(offsetof(struct rgbx_inputs, buttons_pressed) == 320,
               "abi drift: buttons_pressed");
_Static_assert(sizeof(struct rgbx_inputs) == 324, "abi drift: sizeof rgbx_inputs");

_Static_assert(offsetof(struct rgbx_manifest, abi_version) == 0, "abi drift: abi_version");
_Static_assert(offsetof(struct rgbx_manifest, name) == 4, "abi drift: name");
_Static_assert(offsetof(struct rgbx_manifest, width) == 8, "abi drift: width");
_Static_assert(offsetof(struct rgbx_manifest, height) == 12, "abi drift: height");
_Static_assert(offsetof(struct rgbx_manifest, param_count) == 16, "abi drift: param_count");
_Static_assert(offsetof(struct rgbx_manifest, params) == 20, "abi drift: params ptr");
_Static_assert(sizeof(struct rgbx_manifest) == 24, "abi drift: sizeof rgbx_manifest");

_Static_assert(offsetof(struct rgbx_param_desc, name) == 0, "abi drift: desc name");
_Static_assert(offsetof(struct rgbx_param_desc, type) == 4, "abi drift: desc type");
_Static_assert(offsetof(struct rgbx_param_desc, default_value) == 8,
               "abi drift: desc default");
_Static_assert(sizeof(struct rgbx_param_desc) == 12, "abi drift: sizeof rgbx_param_desc");
