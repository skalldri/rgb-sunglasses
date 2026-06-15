#pragma once

#include <cstddef>

/**
 * @brief Status indications for the onboard WS2812 LEDs (led_strip_2).
 */
enum class StatusIndication {
    Off,
    Solid,
    Blinking,      // 1 Hz square wave: 500 ms on / 500 ms off
    Breathing,     // 0.5 Hz sine-wave brightness envelope over a 2 s period (slow)
    FastBreathing, // 1 Hz sine-wave brightness envelope over a 1 s period (fast)
};

/**
 * @brief ROYGBIV color set for status indications.
 */
enum class StatusColor {
    Red,
    Orange,
    Yellow,
    Green,
    Blue,
    Indigo,
    Violet,
};

/**
 * @brief Set the status indication on one of the two onboard LEDs.
 *
 * @param led_index LED index (0 or 1).
 * @param indication The animation to display on this LED.
 * @param color The color to use. Ignored when indication is Off.
 *
 * Thread-safe. No-op when CONFIG_STATUS_LED is not set or led_strip_2 is
 * absent from the device tree.
 */
void status_led_set(size_t led_index, StatusIndication indication, StatusColor color);
