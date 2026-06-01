#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void vm3011_irq(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins);

#ifdef __cplusplus
};
#endif