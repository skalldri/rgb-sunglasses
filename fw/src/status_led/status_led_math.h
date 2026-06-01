#pragma once

/**
 * Pure animation-math helpers for the status_led module.
 *
 * These functions have no Zephyr device-tree or kernel dependencies, so they
 * can be compiled and tested on native_sim without any hardware overlay.
 *
 * status_led.cpp includes this header to share the implementations; the unit
 * test suite includes it directly to exercise the logic in isolation.
 */

#include <status_led/status_led.h>

#include <zephyr/drivers/led_strip.h>

#include <cmath>
#include <cstdint>
#include <numbers>

inline struct led_rgb status_led_color_to_rgb(StatusColor color) {
    switch (color) {
        case StatusColor::Red:    return {.r = 255, .g = 0,   .b = 0};
        case StatusColor::Orange: return {.r = 255, .g = 128, .b = 0};
        case StatusColor::Yellow: return {.r = 255, .g = 255, .b = 0};
        case StatusColor::Green:  return {.r = 0,   .g = 255, .b = 0};
        case StatusColor::Blue:   return {.r = 0,   .g = 0,   .b = 255};
        case StatusColor::Indigo: return {.r = 75,  .g = 0,   .b = 130};
        case StatusColor::Violet: return {.r = 148, .g = 0,   .b = 211};
        default:                  return {.r = 0,   .g = 0,   .b = 0};
    }
}

inline struct led_rgb status_led_scale_brightness(struct led_rgb base, uint8_t brightness) {
    return {
        .r = static_cast<uint8_t>((base.r * brightness) / 255u),
        .g = static_cast<uint8_t>((base.g * brightness) / 255u),
        .b = static_cast<uint8_t>((base.b * brightness) / 255u),
    };
}

/**
 * @brief Blinking brightness at a given tick.
 *
 * Returns 255 for the first half of the period and 0 for the second half.
 *
 * @param tick          Current tick counter.
 * @param half_period   Number of ticks for each on/off half-period.
 */
inline uint8_t status_led_blinking_brightness(uint32_t tick, uint32_t half_period) {
    return (tick % (half_period * 2u) < half_period) ? 255u : 0u;
}

/**
 * @brief Breathing brightness at a given tick.
 *
 * Maps a sine wave (period = `period_ticks` ticks) to the range [0, 255].
 *
 * @param tick          Current tick counter.
 * @param period_ticks  Total ticks for one full sine cycle.
 */
inline uint8_t status_led_breathing_brightness(uint32_t tick, uint32_t period_ticks) {
    float phase = (2.0f * std::numbers::pi_v<float> * (tick % period_ticks)) /
                  static_cast<float>(period_ticks);
    float sine = (sinf(phase) + 1.0f) * 0.5f;  // map -1..1 → 0..1
    return static_cast<uint8_t>(sine * 255.0f);
}
