/*
 * Copyright (c) 2024 Stuart Alldritt
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

/** IPC endpoint name for netcore version queries. */
#define NETCORE_VER_ENDPOINT_NAME "NETCORE_VER"

/** Request command byte sent by the app core to request version info. */
#define NETCORE_VER_CMD_GET 0x01

/**
 * @brief Response payload sent by the net core over the NETCORE_VER IPC endpoint.
 *
 * Matches the MCUboot image version fields so the app core can populate
 * the simulated net-core flash slot presented to MCUMgr.
 */
struct netcore_version_rsp {
	/** Firmware major version (from ipc_radio VERSION file). */
	uint8_t major;
	/** Firmware minor version. */
	uint8_t minor;
	/** Firmware patchlevel / revision. */
	uint16_t revision;
	/** Firmware tweak / build number. */
	uint32_t build_num;
	/** SHA-256 hash of the running image, or all-zeros if unavailable. */
	uint8_t sha256[32];
} __packed; /* 40 bytes total */
