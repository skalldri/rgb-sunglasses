#pragma once

#include <animations/animation_types.h>

enum class Indicator {
    None = 0,
    BtAdvertising,
    BtConnecting,
};

int pattern_controller_request_indicator(Indicator ind);

int pattern_controller_reset_indicator();

int pattern_controller_change_to_animation(Animation animation);

Animation pattern_controller_get_current_animation(void);
