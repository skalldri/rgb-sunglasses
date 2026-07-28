#pragma once

#include <cstdint>

/**
 * @brief Accessors for the dedicated Shuffle GATT service (issue #243), which owns the
 * global shuffle settings previously housed in the Core Config service. Deliberately
 * plain functions consumed through pattern_controller's ShuffleConfigSource seam —
 * widening ConfigurationProvider would only force updates to its test fakes.
 */

/** @brief Whether shuffle mode is currently enabled (BLE-settable, persisted). */
bool shuffle_service_get_enabled(void);

/** @brief Shuffle dwell lower bound in seconds. May exceed the max — tolerated;
 *  ShuffleController swaps at pick time. */
uint32_t shuffle_service_get_min_duration_s(void);

/** @brief Shuffle dwell upper bound in seconds (see the min getter's swap note). */
uint32_t shuffle_service_get_max_duration_s(void);

/** @brief Shell convenience (anim shuffle on|off): assigns the characteristic, which
 *  notifies subscribers and persists like a BLE write. */
void shuffle_service_set_enabled(bool enabled);
