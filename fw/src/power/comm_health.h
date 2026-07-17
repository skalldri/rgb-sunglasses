#pragma once

#include <stdint.h>

/**
 * @brief Pure comm-health hysteresis for the charger status thread: N
 * consecutive failed reads enter the error state, one success exits.
 *
 * BT-free and kernel-free so fw/tests/power/comm_health covers it on
 * native_sim; power.cpp owns the side effects (status LED, BLE sentinel,
 * poll backoff) keyed off the returned event.
 */

enum class CommHealthEvent : uint8_t {
    None = 0,     /**< No state change this tick. */
    EnteredError, /**< Crossed the consecutive-failure threshold (fires once). */
    Recovered,    /**< First successful read after being in the error state. */
};

struct CommHealth {
    uint32_t fail_ticks = 0; /**< Consecutive failed ticks so far. */
    bool in_error = false;   /**< Currently in the comm-error state. */
};

/**
 * @brief Advance the health state by one poll tick.
 *
 * @param health      State to advance.
 * @param read_ok     Whether this tick's charger reads all succeeded.
 * @param entry_ticks Consecutive failures required to enter the error state.
 * @return The transition event (None for steady states).
 */
CommHealthEvent comm_health_tick(CommHealth &health, bool read_ok, uint32_t entry_ticks);
