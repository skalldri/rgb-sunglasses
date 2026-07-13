#include "bq25792_init.h"
#include "bq25792_irq.h"

#include <zephyr/drivers/bq25792/bq25792.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(bq25792);

#define DT_DRV_COMPAT ti_bq25792

static int bq25792_init(const struct device *dev) {
    const struct bq25792_dev_config *cfg = dev->config;
    struct bq25792_dev_data *data = dev->data;
    data->dev = dev;
    k_mutex_init(&data->xact_mutex);

    if (cfg->int_gpio.port) {
        gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(&cfg->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&data->callback, bq25792_irq, BIT(cfg->int_gpio.pin));
        gpio_add_callback(cfg->int_gpio.port, &data->callback);

        LOG_INF("Interrupt pin configured! Port %s, pin %d", cfg->int_gpio.port->name,
                cfg->int_gpio.pin);
    }

    return 0;
}

#define BQ25792_DEFINE(inst)                                                                      \
    static struct bq25792_dev_data bq25792_data_##inst;                                           \
                                                                                                  \
    static const struct bq25792_dev_config bq25792_config_##inst = {                              \
        .i2c = I2C_DT_SPEC_INST_GET(inst),                                                        \
        .int_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, irq_gpios, {0})};                              \
                                                                                                  \
    DEVICE_DT_INST_DEFINE(inst, bq25792_init, NULL, &bq25792_data_##inst, &bq25792_config_##inst, \
                          POST_KERNEL, CONFIG_BQ25792_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(BQ25792_DEFINE)