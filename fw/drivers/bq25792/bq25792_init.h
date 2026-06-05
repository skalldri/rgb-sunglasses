#pragma once

#include <zephyr/drivers/bq25792/bq25792.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>

struct bq25792_dev_data {
    struct gpio_callback callback;
    const struct device *dev;
    bq25792_irq_callback_t irq_cb;
    void *irq_cb_user_data;
};

struct bq25792_dev_config {
    struct i2c_dt_spec i2c;
    struct gpio_dt_spec int_gpio;
};