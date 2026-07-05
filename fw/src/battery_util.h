#pragma once

#include <stdint.h>

/**
 * @brief Quantizes a measurement to the nearest multiple of @p step.
 *
 * BT-free helper for the battery monitoring service: the BQ25792's ADC
 * readings jitter by a few LSB between samples, and the GATT characteristic
 * assignment operator notifies on every value *change* — publishing raw
 * readings would notify subscribed BLE clients on every 500 ms sample.
 * Quantizing to a step below the display resolution (10 mV / 10 mA) keeps
 * idle telemetry silent without losing anything the app would show.
 *
 * Rounds to nearest, symmetric around zero (so -14 with step 10 -> -10,
 * -15 -> -20), preserving the sign that encodes charge/discharge direction.
 *
 * @param value Raw measurement.
 * @param step  Quantization step; values <= 1 return @p value unchanged.
 * @return @p value rounded to the nearest multiple of @p step.
 */
constexpr int32_t battery_quantize(int32_t value, int32_t step) {
    if (step <= 1) {
        return value;
    }

    const int32_t half = step / 2;
    if (value >= 0) {
        return ((value + half) / step) * step;
    }
    return -(((-value + half) / step) * step);
}
