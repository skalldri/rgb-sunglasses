#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>

/* Must cover the largest chain-length in boards/native_sim.overlay (216). */
#define FAKE_LED_STRIP_MAX_PIXELS 216

#ifdef __cplusplus
extern "C" {
#endif

/* Number of led_strip_update_rgb() calls this fake strip has received. */
uint32_t fake_led_strip_update_count(const struct device *dev);

/* Pixel data from the most recent update; *len is its pixel count (0 if the
 * strip was never updated). The pointer stays valid for the device lifetime. */
const struct led_rgb *fake_led_strip_last_frame(const struct device *dev, size_t *len);

/* Clear the update counter and captured frame. */
void fake_led_strip_reset(const struct device *dev);

#ifdef __cplusplus
}
#endif
