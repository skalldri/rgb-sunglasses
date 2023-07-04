#pragma once

#include <led_config.h>
#include <cstdint>
#include <cstddef>

const LedConfig* get_current_led_config();

int claimBufferForRender(size_t& buffer);

int releaseBufferFromRender(const size_t buffer);

int set_pixel_in_framebuffer(const LedConfig* config, size_t x, size_t y, size_t bufferId, uint8_t red, uint8_t green, uint8_t blue);