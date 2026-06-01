#pragma once

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>

struct vm3011_dev_data {
    struct gpio_callback callback;
};

struct vm3011_dev_config {
    struct i2c_dt_spec i2c;
    struct gpio_dt_spec int_gpio;
};