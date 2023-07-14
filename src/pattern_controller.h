#pragma once

#include <animations/animation_types.h>
#include <led_config.h>

enum class Indicator {
    None = 0,
    BtAdvertising,
    BtConnecting,
    BtPairing,
};

int pattern_controller_request_indicator(Indicator ind);

int pattern_controller_reset_indicator();

int pattern_controller_change_to_animation(Animation animation);

Animation pattern_controller_get_current_animation(void);

int pattern_controller_set_pixel_in_framebuffer(const LedConfig* config, size_t x, size_t y, size_t bufferId, uint8_t red, uint8_t green, uint8_t blue);
