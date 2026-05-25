/*
 * Copyright (c) 2024 Stuart Alldritt
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <fw_info_bare.h>
#include <pm_config.h>
#include <tinycrypt/sha256.h>
#include <app_version.h>
#include "netcore_version_protocol.h"

LOG_MODULE_REGISTER(netcore_version, CONFIG_NETCORE_VERSION_LOG_LEVEL);

static struct ipc_ept ver_ept;

/**
 * @brief Compute SHA-256 of the running net core image payload.
 *
 * The image in the ipc_radio partition is the raw firmware payload written
 * by PCD during an OTA update (or by the programmer on initial flash). MCUboot
 * strips its own header and TLV area before calling PCD, so there is never
 * an IMAGE_MAGIC or TLV at the ipc_radio partition start. Specifically,
 * nrf53_hooks.c::network_core_update() skips the MCUboot header with:
 *
 *   vtable_addr = (uint32_t)hdr + hdr->ih_hdr_size;
 *   pcd_network_core_update(vtable, hdr->ih_img_size);
 *
 * Only hdr->ih_img_size bytes of raw payload are written to net core flash.
 *
 * Every b0n-managed net core image embeds a fw_info struct at
 * CONFIG_FW_INFO_OFFSET (0x200) from the start of the image. fw_info.size
 * gives the exact byte count of the firmware. The SHA-256 here covers
 * [PM_IPC_RADIO_ADDRESS, PM_IPC_RADIO_ADDRESS + fw_info.size), i.e. the raw
 * payload only.
 *
 * NOTE: This hash does NOT match the SHA-256 stored in the MCUboot TLV of the
 * original DFU package. MCUboot's hash covers (header + payload + protected
 * TLV), whereas ours covers only the payload. The header is permanently
 * discarded by PCD and cannot be recovered from net core flash. This hash is
 * still useful as a unique build fingerprint: it will change with every
 * firmware update. To reproduce it offline, strip the MCUboot header from the
 * net core .bin (skip the first ih_hdr_size bytes) and compute sha256sum on
 * the remaining ih_img_size bytes.
 *
 * The net core's own internal flash is memory-mapped (XIP), so we can hash
 * it directly without needing the flash driver.
 */
static void compute_sha256(uint8_t sha256[32])
{
	const struct fw_info *finfo;
	struct tc_sha256_state_struct sha_state;

	/*
	 * fw_info_find() searches for the fw_info magic at the allowed offsets
	 * from PM_IPC_RADIO_ADDRESS (the XIP address of our partition start).
	 */
	finfo = fw_info_find(PM_IPC_RADIO_ADDRESS);
	if (finfo == NULL) {
		LOG_WRN("fw_info not found in ipc_radio partition, hash unavailable");
		return;
	}

	if (finfo->size == 0 || finfo->size > PM_IPC_RADIO_SIZE) {
		LOG_WRN("fw_info.size=0x%x out of range (max 0x%x), hash unavailable",
			finfo->size, (uint32_t)PM_IPC_RADIO_SIZE);
		return;
	}

	LOG_DBG("Computing SHA-256 over %u bytes at 0x%08x", finfo->size, PM_IPC_RADIO_ADDRESS);

	tc_sha256_init(&sha_state);
	tc_sha256_update(&sha_state, (const uint8_t *)PM_IPC_RADIO_ADDRESS, finfo->size);
	tc_sha256_final(sha256, &sha_state);

	LOG_DBG("SHA-256 computed successfully");
}

static void ver_recv_cb(const void *data, size_t len, void *priv)
{
	struct netcore_version_rsp rsp;
	int err;

	if (len < 1 || ((const uint8_t *)data)[0] != NETCORE_VER_CMD_GET) {
		LOG_WRN("Unexpected request (len=%zu, cmd=0x%02x)", len,
			len >= 1 ? ((const uint8_t *)data)[0] : 0);
		return;
	}

	memset(&rsp, 0, sizeof(rsp));
	rsp.major     = APP_VERSION_MAJOR;
	rsp.minor     = APP_VERSION_MINOR;
	rsp.revision  = APP_PATCHLEVEL;
	rsp.build_num = APP_TWEAK;

	compute_sha256(rsp.sha256);

	err = ipc_service_send(&ver_ept, &rsp, sizeof(rsp));
	if (err < 0) {
		LOG_ERR("Failed to send version response: %d", err);
	} else {
		LOG_INF("Sent version %u.%u.%u+%u to app core",
			rsp.major, rsp.minor, rsp.revision, rsp.build_num);
	}
}

static void ver_bound_cb(void *priv)
{
	LOG_INF("Version IPC endpoint bound");
}

static void ver_unbound_cb(void *priv)
{
	/* App core reset or IPC link lost. The endpoint will rebind automatically
	 * when the app core re-registers the same endpoint name. */
	LOG_WRN("Version IPC endpoint unbound");
}

static const struct ipc_ept_cfg ver_ept_cfg = {
	.name = NETCORE_VER_ENDPOINT_NAME,
	.cb = {
		.bound    = ver_bound_cb,
		.received = ver_recv_cb,
		.unbound  = ver_unbound_cb,
	},
};

static int netcore_version_init(void)
{
	const struct device *ipc = DEVICE_DT_GET(DT_CHOSEN(zephyr_bt_hci_ipc));
	int err;

	err = ipc_service_open_instance(ipc);
	if (err < 0 && err != -EALREADY) {
		LOG_ERR("Failed to open IPC instance: %d", err);
		return err;
	}

	err = ipc_service_register_endpoint(ipc, &ver_ept, &ver_ept_cfg);
	if (err < 0) {
		LOG_ERR("Failed to register version endpoint: %d", err);
		return err;
	}

	return 0;
}

SYS_INIT(netcore_version_init, APPLICATION, 0);
