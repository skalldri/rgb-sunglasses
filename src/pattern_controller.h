#pragma once

#include <animations/animation.h>

enum class Indicator {
    None = 0,
    BtAdvertising,
    BtConnecting,
};

int pattern_controller_request_indicator(Indicator ind);

int pattern_controller_reset_indicator();
