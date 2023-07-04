#pragma once

enum class SystemIndicators {
    BT_ADVERTISING, // We are advertising to the world and anyone can connect with us
    BT_CONNECTING, // We are attempting to connect with a single peer
    // Others later?
};

// LED Controller has a few responsibilities:
// - User indication: if no BT device is connected, 

// Submit a request to play an indicator pattern.
// Indicator patterns are queued up and played in-order

int led_controller_request_indication(SystemIndicators indicator);