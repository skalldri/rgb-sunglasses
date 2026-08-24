/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Stuart Alldritt
 *
 * Simulator shim for <zephyr/logging/log.h> — just enough for the WASM
 * build of audio_dsp.cpp (LOG_MODULE_REGISTER + level macros as no-ops;
 * the only call site is a CONFIG_DEBUG-gated LOG_DBG that is compiled out
 * here anyway).
 */

#ifndef RGBX_SIM_SHIM_ZEPHYR_LOG_H_
#define RGBX_SIM_SHIM_ZEPHYR_LOG_H_

#define LOG_MODULE_REGISTER(...)
#define LOG_MODULE_DECLARE(...)

#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_DBG(...) ((void)0)

#endif /* RGBX_SIM_SHIM_ZEPHYR_LOG_H_ */
