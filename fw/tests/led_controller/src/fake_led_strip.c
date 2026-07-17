/*
 * Fake led_strip driver (compatible "test,fake-led-strip"): records every
 * update_rgb call so tests can assert whether — and with what pixel data —
 * the display thread is clocking frames out. Sanctioned mock style (c),
 * a DT-registered device emulator; see /add-fw-test.
 */

#define DT_DRV_COMPAT test_fake_led_strip

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/sys/util.h>

#include "fake_led_strip.h"

struct fake_led_strip_config {
    size_t length;
};

struct fake_led_strip_data {
    uint32_t update_count;
    struct led_rgb last_frame[FAKE_LED_STRIP_MAX_PIXELS];
    size_t last_len;
};

static int fake_update_rgb(const struct device *dev, struct led_rgb *pixels, size_t num_pixels) {
    struct fake_led_strip_data *data = dev->data;
    size_t n = MIN(num_pixels, (size_t)FAKE_LED_STRIP_MAX_PIXELS);

    memcpy(data->last_frame, pixels, n * sizeof(struct led_rgb));
    data->last_len = n;
    data->update_count++;
    return 0;
}

static size_t fake_length(const struct device *dev) {
    const struct fake_led_strip_config *config = dev->config;

    return config->length;
}

static const struct led_strip_driver_api fake_led_strip_api = {
    .update_rgb = fake_update_rgb,
    .length     = fake_length,
};

uint32_t fake_led_strip_update_count(const struct device *dev) {
    const struct fake_led_strip_data *data = dev->data;

    return data->update_count;
}

const struct led_rgb *fake_led_strip_last_frame(const struct device *dev, size_t *len) {
    const struct fake_led_strip_data *data = dev->data;

    *len = data->last_len;
    return data->last_frame;
}

void fake_led_strip_reset(const struct device *dev) {
    struct fake_led_strip_data *data = dev->data;

    memset(data, 0, sizeof(*data));
}

#define FAKE_LED_STRIP_DEFINE(inst)                                                       \
    static struct fake_led_strip_data fake_led_strip_data_##inst;                         \
    static const struct fake_led_strip_config fake_led_strip_config_##inst = {            \
        .length = DT_INST_PROP(inst, chain_length),                                       \
    };                                                                                    \
    DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &fake_led_strip_data_##inst,                  \
                          &fake_led_strip_config_##inst, POST_KERNEL,                     \
                          CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fake_led_strip_api);

DT_INST_FOREACH_STATUS_OKAY(FAKE_LED_STRIP_DEFINE)
