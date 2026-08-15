#pragma once

#include <configuration_provider.h>
#include <led_config.h>
#include <zephyr/kernel.h>

#include <cstddef>
#include <cstdint>

const LedConfig* get_current_led_config();

int claimBufferForRender(size_t& buffer);

int releaseBufferFromRender(const size_t buffer);

/**
 * @brief Block until the display thread has consumed a frame (issue #379).
 *
 * Given once per display cycle (coalesced: max one credit). The render thread
 * takes this to pace itself off the display clock, so every display push
 * samples exactly one fresh frame — no free-running second clock, no phase
 * slip. Always pass a bounded timeout, never K_FOREVER: the display thread
 * exits at boot if the LED strip devices are not ready, and the render thread
 * must fall back to self-pacing rather than wedge.
 *
 * @return 0 when a frame was consumed since the last take, -EAGAIN/-EBUSY on
 *         timeout (k_sem_take semantics).
 */
int led_controller_wait_frame_consumed(k_timeout_t timeout);

int set_pixel_in_framebuffer(const LedConfig* config, size_t x, size_t y, size_t bufferId,
                             uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Inject a ConfigurationProvider for the LED display thread.
 *
 * If not called before the thread reads configuration, CoreConfig::getInstance()
 * is used as the default. Useful for unit tests.
 */
void led_controller_set_config_provider(ConfigurationProvider* provider);