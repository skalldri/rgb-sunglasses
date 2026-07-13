#pragma once

#include <zephyr/drivers/bq25792/bq25792.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

struct bq25792_dev_data {
    struct gpio_callback callback;
    const struct device *dev;
    bq25792_irq_callback_t irq_cb;
    void *irq_cb_user_data;
    /* Serializes multi-transfer register sequences (read-modify-write,
     * write + read-back-verify). The TPS25750 I2Cm bridge's task_mutex only
     * covers a single transfer — see the header comment in bq25792.cpp and
     * the multi-step transaction rule in fw/CLAUDE.md. */
    struct k_mutex xact_mutex;
};

struct bq25792_dev_config {
    struct i2c_dt_spec i2c;
    struct gpio_dt_spec int_gpio;
};