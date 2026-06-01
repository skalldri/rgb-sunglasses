#pragma once

#include <configuration_provider.h>
#include <led_config.h>

#include <cstddef>
#include <cstdint>

const LedConfig* get_current_led_config();

int claimBufferForRender(size_t& buffer);

int releaseBufferFromRender(const size_t buffer);

int set_pixel_in_framebuffer(const LedConfig* config, size_t x, size_t y, size_t bufferId,
                             uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Inject a ConfigurationProvider for the LED display thread.
 *
 * If not called before the thread reads configuration, CoreConfig::getInstance()
 * is used as the default. Useful for unit tests.
 */
void led_controller_set_config_provider(ConfigurationProvider* provider);