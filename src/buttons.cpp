#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(buttons);

static const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec button1 = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
static const struct gpio_dt_spec button2 = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios);
static const struct gpio_dt_spec button3 = GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios);
static const struct gpio_dt_spec button_wake = GPIO_DT_SPEC_GET(DT_ALIAS(sw_wake), gpios);

void button_callback(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
    LOG_INF("Button ISR triggered!");
    return;
}

static int button_init(const struct device *dev)
{
    gpio_pin_configure_dt(&button0, GPIO_INPUT);
    gpio_pin_configure_dt(&button1, GPIO_INPUT);
    gpio_pin_configure_dt(&button2, GPIO_INPUT);
    gpio_pin_configure_dt(&button3, GPIO_INPUT);
    gpio_pin_configure_dt(&button_wake, GPIO_INPUT);

    // Configure GPIO pin interrupts
    gpio_pin_interrupt_configure_dt(&button0, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_pin_interrupt_configure_dt(&button1, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_pin_interrupt_configure_dt(&button2, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_pin_interrupt_configure_dt(&button3, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_pin_interrupt_configure_dt(&button_wake, GPIO_INT_EDGE_TO_ACTIVE);

    static struct gpio_callback callback0;
    static struct gpio_callback callback1;
    static struct gpio_callback callback2;
    static struct gpio_callback callback3;
    static struct gpio_callback callback_wake;

    gpio_init_callback(&callback0, button_callback, BIT(button0.pin));
    gpio_init_callback(&callback1, button_callback, BIT(button1.pin));
    gpio_init_callback(&callback2, button_callback, BIT(button2.pin));
    gpio_init_callback(&callback3, button_callback, BIT(button3.pin));
    gpio_init_callback(&callback_wake, button_callback, BIT(button_wake.pin));

    gpio_add_callback(button0.port, &callback0);
    gpio_add_callback(button1.port, &callback1);
    gpio_add_callback(button2.port, &callback2);
    gpio_add_callback(button3.port, &callback3);
    gpio_add_callback(button_wake.port, &callback_wake);

    return 0;
}

SYS_INIT(button_init, APPLICATION, 1);