#pragma once

#include <configuration_provider.h>
#include <singleton.h>

#include <cstddef>
#include <cstdint>

class CoreConfig : public Singleton<CoreConfig>, public ConfigurationProvider {
   public:
    static constexpr size_t kServiceIdNum = 1;

    /**
     * @brief Returns a value between 0 and 1 representing the current display brightness
     *
     * @return float
     */
    float getBrightnessFactor() override;

    float getDisplayRateMs() override;

    float getRenderRateMs() override;

    /**
     * @brief Returns a value between 0 and 1 representing the status LED brightness.
     */
    float getStatusLedBrightnessFactor();

    // Shuffle mode (issue #121). Deliberately NOT part of ConfigurationProvider: shuffle
    // logic consumes these through its own ShuffleConfigSource seam, so widening the
    // interface would only force updates to its test fakes (same reasoning as
    // getStatusLedBrightnessFactor() above). Non-const like every getter here — see the
    // CoreConfig note in fw/CLAUDE.md.

    /** @brief Whether shuffle mode is currently enabled (BLE-settable, persisted). */
    bool getShuffleEnabled();
    /** @brief Shuffle dwell lower bound in seconds. May exceed the max — tolerated;
     *  ShuffleController swaps at pick time. */
    uint32_t getShuffleMinDurationS();
    /** @brief Shuffle dwell upper bound in seconds (see the min getter's swap note). */
    uint32_t getShuffleMaxDurationS();
    /** @brief Shell convenience (anim shuffle on|off): assigns the characteristic, which
     *  notifies subscribers and persists like a BLE write. */
    void setShuffleEnabled(bool enabled);
};