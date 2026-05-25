/*
 * Copyright (c) 2024 Stuart Alldritt
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/init.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <app_version.h>
#include "netcore_version_protocol.h"

LOG_MODULE_REGISTER(netcore_version, CONFIG_NETCORE_VERSION_LOG_LEVEL);

/*
 * Minimal MCUboot image-header and TLV definitions.
 * We avoid depending on CONFIG_MCUBOOT_BOOTUTIL_LIB (not available on the net
 * core) and instead replicate only the fields we need.
 */
#define MCUBOOT_IMAGE_MAGIC    0x96f3b83dUL
#define MCUBOOT_TLV_INFO_MAGIC 0x6907U
#define MCUBOOT_TLV_SHA256     0x0010U

struct mcuboot_image_header {
	uint32_t ih_magic;
	uint32_t ih_load_addr;
	uint16_t ih_hdr_size;
	uint16_t ih_protect_tlv_size;
	uint32_t ih_img_size;
	uint32_t ih_flags;
	struct {
		uint8_t  iv_major;
		uint8_t  iv_minor;
		uint16_t iv_revision;
		uint32_t iv_build_num;
	} ih_ver;
	uint32_t _pad1;
} __packed;

struct mcuboot_tlv_info {
	uint16_t it_magic;
	uint16_t it_tlv_tot;
} __packed;

struct mcuboot_tlv {
	uint16_t it_type;
	uint16_t it_len;
} __packed;

static struct ipc_ept ver_ept;

/**
 * @brief Attempt to read SHA-256 from an MCUboot TLV in slot0_partition.
 *
 * The image in slot0_partition may be an MCUboot-wrapped image (after OTA) or a
 * b0n-only signed binary (initial programming). If the MCUboot IMAGE_MAGIC is
 * not found the output is left as all-zeros.
 */
static void read_sha256_from_slot0(uint8_t sha256[32])
{
	const struct device *flash_dev = FIXED_PARTITION_DEVICE(ipc_radio);
	const off_t base = FIXED_PARTITION_OFFSET(ipc_radio);
	struct mcuboot_image_header hdr;
	struct mcuboot_tlv_info tlv_info;
	struct mcuboot_tlv tlv;
	off_t tlv_start;
	off_t pos;
	off_t tlv_end;

	if (!device_is_ready(flash_dev)) {
		LOG_WRN("Flash device not ready, hash unavailable");
		return;
	}

	if (flash_read(flash_dev, base, &hdr, sizeof(hdr)) != 0) {
		LOG_WRN("Failed to read slot0_partition header");
		return;
	}

	if (hdr.ih_magic != MCUBOOT_IMAGE_MAGIC) {
		/* Initial programming uses b0n format (no MCUboot header). */
		LOG_DBG("No MCUboot header in slot0_partition (b0n format), hash unavailable");
		return;
	}

	/*
	 * The unprotected TLV area starts after the firmware image payload.
	 * If a protected TLV block is present it comes first; skip it.
	 */
	tlv_start = base + hdr.ih_hdr_size + hdr.ih_img_size + hdr.ih_protect_tlv_size;

	if (flash_read(flash_dev, tlv_start, &tlv_info, sizeof(tlv_info)) != 0) {
		return;
	}

	if (tlv_info.it_magic != MCUBOOT_TLV_INFO_MAGIC) {
		return;
	}

	pos = tlv_start + sizeof(tlv_info);
	tlv_end = tlv_start + tlv_info.it_tlv_tot;

	while (pos < tlv_end) {
		if (flash_read(flash_dev, pos, &tlv, sizeof(tlv)) != 0) {
			break;
		}
		if (tlv.it_type == MCUBOOT_TLV_SHA256 && tlv.it_len == 32) {
			if (flash_read(flash_dev, pos + sizeof(tlv), sha256, 32) == 0) {
				LOG_DBG("SHA-256 read successfully from slot0_partition TLV");
			}
			return;
		}
		pos += sizeof(tlv) + tlv.it_len;
	}

	LOG_DBG("SHA-256 TLV not found in slot0_partition");
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

	read_sha256_from_slot0(rsp.sha256);

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
