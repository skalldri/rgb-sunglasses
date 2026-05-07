/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <soc.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main);

void main(void)
{
    LOG_INF("RGB Sunglasses - Main");

    for (;;) {
        // LOG_INF("App is alive");
        k_msleep(10000);
    }
}
