#include <zephyr/init.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "vm3011_init.h"
#include "vm3011_irq.h"

#define DT_DRV_COMPAT vesper_vm3011

LOG_MODULE_DECLARE(vm3011);

static int vm3011_init(const struct device *dev) {

    const struct vm3011_dev_config *cfg = (struct vm3011_dev_config*)dev->config;
    struct vm3011_dev_data* data = (struct vm3011_dev_data*)dev->data;

    if (cfg->int_gpio.port)
    {
        gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(&cfg->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&data->callback, vm3011_irq, BIT(cfg->int_gpio.pin));
        gpio_add_callback(cfg->int_gpio.port, &data->callback);

        LOG_INF("Interrupt pin configured! Port %s, pin %d", cfg->int_gpio.port->name, cfg->int_gpio.pin);
    }

    return 0;
}

#define VM3011_DEFINE(inst)                                                             \
    static struct vm3011_dev_data vm3011_data_##inst;                                   \
                                                                                        \
                                                                                        \
    static const struct vm3011_dev_config vm3011_config_##inst =                        \
        {                                                                               \
            .i2c = I2C_DT_SPEC_INST_GET(inst),                                          \
            .int_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, irq_gpios, {0})                  \
        };                                                                              \
                                                                                        \
    DEVICE_DT_INST_DEFINE(inst, vm3011_init, NULL,                                      \
                      &vm3011_data_##inst, &vm3011_config_##inst,                       \
                      APPLICATION, CONFIG_VM3011_INIT_PRIORITY, NULL);
    

DT_INST_FOREACH_STATUS_OKAY(VM3011_DEFINE)