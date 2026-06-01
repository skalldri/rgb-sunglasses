/*
 * Copyright (c) 2024 Stuart Alldritt
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <netcore_version.h>

LOG_MODULE_REGISTER(netcore_version, CONFIG_NETCORE_VERSION_LOG_LEVEL);

static struct ipc_ept ver_ept;

/* Version cache: populated once when the net core responds.
 * The atomic flag acts as the happens-before barrier so the struct is safe to
 * read after atomic_get() returns non-zero. */
static struct netcore_version_rsp cached_rsp;
static atomic_t version_ready = ATOMIC_INIT(0);

int netcore_version_get(struct netcore_version_rsp *out)
{
	if (!atomic_get(&version_ready)) {
		return -EAGAIN;
	}
	/* Data is write-once; safe to read without a lock once flag is set. */
	memcpy(out, &cached_rsp, sizeof(*out));
	return 0;
}

static void ver_bound_cb(void *priv)
{
	const uint8_t req = NETCORE_VER_CMD_GET;
	int err;

	LOG_INF("Version IPC endpoint bound, requesting net core version");

	err = ipc_service_send(&ver_ept, &req, sizeof(req));
	if (err < 0) {
		LOG_ERR("Failed to send version request: %d", err);
	}
}

static void ver_recv_cb(const void *data, size_t len, void *priv)
{
	if (len != sizeof(struct netcore_version_rsp)) {
		LOG_ERR("Unexpected response length %zu (expected %zu)", len,
			sizeof(struct netcore_version_rsp));
		return;
	}

	memcpy(&cached_rsp, data, sizeof(cached_rsp));
	/* Publish the write to cached_rsp before setting the flag. */
	atomic_set(&version_ready, 1);

	LOG_INF("Net core version: %u.%u.%u+%u",
		cached_rsp.major, cached_rsp.minor,
		cached_rsp.revision, cached_rsp.build_num);
}

static void ver_unbound_cb(void *priv)
{
	/* Net core reset or IPC link lost; clear cached data so stale info is
	 * not returned after a potential net core firmware update. */
	atomic_set(&version_ready, 0);
	memset(&cached_rsp, 0, sizeof(cached_rsp));
	LOG_WRN("Version IPC endpoint unbound; cached version cleared");
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
	const struct device *ipc = DEVICE_DT_GET(DT_NODELABEL(ipc0));
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
