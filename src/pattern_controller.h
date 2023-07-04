#pragma once

enum Indicator {
    NONE,
    BT_ADVERTISING,
    BT_CONNECTING
};

int pattern_controller_request_indicator(Indicator ind);

int pattern_controller_reset_indicator();
