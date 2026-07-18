/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include "ipc_bt.h"

LOG_MODULE_REGISTER(ipc_radio, CONFIG_IPC_RADIO_LOG_LEVEL);

#if !(CONFIG_IPC_RADIO_802154 || CONFIG_IPC_RADIO_BT)
#error "No radio serialization selected."
#endif

/* proto0's 32k crystal is out of its assumed accuracy class, so the controller's
 * RX window widening (Core v5 Vol 6 Part B 4.5.7) is too narrow at long connection
 * intervals. The appcore conn-param governor's SLOW set only survives supervision
 * timeout because the netcore declares the sleep clock as 500ppm-class and therefore
 * widens honestly - that declaration lives in the per-image board fragment
 * fw/sysbuild/ipc_radio/boards/rgb_sunglasses_proto0_nrf5340_cpunet.conf. A newly-added
 * per-image fragment can silently fail to apply on an incremental build (see
 * fw/CLAUDE.md), which shipped a netcore that dropped every idle downgrade with
 * "Disconnected (reason 8)" (issues #188 / #199). Fail the proto0 netcore build instead.
 * Gated to proto0: the legacy DK board (dk-support branch, governor compiled out) has
 * no such requirement.
 */
#if defined(CONFIG_BOARD_RGB_SUNGLASSES_PROTO0_NRF5340_CPUNET)
BUILD_ASSERT(IS_ENABLED(CONFIG_CLOCK_CONTROL_NRF_K32SRC_500PPM),
	     "proto0 netcore requires CONFIG_CLOCK_CONTROL_NRF_K32SRC_500PPM "
	     "(fw/sysbuild/ipc_radio/boards/rgb_sunglasses_proto0_nrf5340_cpunet.conf) or "
	     "the conn-param governor's SLOW set drops the link on every idle downgrade");
#endif

int main(void)
{
	int err;

	err = ipc_bt_init();
	if ((err) && (err != -ENOSYS)) {
		LOG_ERR("Error initializing ipc radio %d", err);
		return err;
	}

	for (;;) {
		err = ipc_bt_process();

		if (err == -ENOSYS) {
			/* Particular implementation does not need the process function */
			return 0;
		} else if (err) {
			LOG_ERR("Error processing ipc radio %d", err);
			return err;
		}
	}
}
