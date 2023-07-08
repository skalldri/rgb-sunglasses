#include <zephyr/drivers/vm3011/vm3011.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(vm3011, LOG_LEVEL_INF);

#include "vm3011_priv.h"
#include "vm3011_irq.h"

void vm3011_irq(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins) {
    LOG_INF("Got a VM3011 callback!");
}

/* X-Macro Definition for vm3011_dump() function */
#define REG(_regName) \
    { \
        _regName tmp(cfg); \
        tmp.dump(); \
    }

int vm3011_dump(const struct device *dev) {
    if (!dev)
    {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct vm3011_dev_config *cfg = (const struct vm3011_dev_config*)dev->config;

    if (!device_is_ready(cfg->i2c.bus))
    {
        LOG_ERR("bus not ready");
        return -ENODEV;
    }

    REG_LIST

    return 0;
}

int vm3011_clear_dout(const struct device *dev) {
    if (!dev)
    {
        LOG_ERR("NULL-device pointer");
        return -ENODEV;
    }

    const struct vm3011_dev_config *cfg = (const struct vm3011_dev_config*)dev->config;

    VM3011_I2C_CNTRL reg(cfg);
    
    // Write 1 to clear DOUT
    return reg.set<VM3011_I2C_CNTRL_DOUT_CLEAR>(1, true /* flush */);
}