#pragma once
#include <zephyr/kernel.h>

#include <cstdint>

#include "audio_dsp.h"

extern struct k_msgq audio_result_q;

/**
 * @brief Runtime-tunable AGC parameters. Decouples sound.cpp from any concrete
 * BT/Settings-backed implementation - see AudioDspConfigProvider in audio_dsp.h for the
 * full rationale (same seam, applied to the AGC loop + its shell commands instead of
 * audio_dsp_process()). Unlike AudioDspConfigProvider, this interface also has setters:
 * the existing "sound agc target-low/target-high/rate" shell commands both read *and*
 * write through whichever provider is currently set.
 */
class AgcConfigProvider {
   public:
    virtual ~AgcConfigProvider() = default;

    /** Increase gain when smoothed RMS falls below this. */
    virtual float getTargetLow() = 0;
    virtual void setTargetLow(float value) = 0;

    /** Decrease gain when smoothed RMS exceeds this. */
    virtual float getTargetHigh() = 0;
    virtual void setTargetHigh(float value) = 0;

    /** Minimum frames between gain adjustments. */
    virtual uint32_t getRateLimitFrames() = 0;
    virtual void setRateLimitFrames(uint32_t value) = 0;
};

/**
 * @brief Sets the provider the AGC loop and shell commands read/write through.
 *
 * Pass nullptr to revert to the built-in default (historical static-variable values).
 */
void sound_set_agc_config_provider(AgcConfigProvider *provider);
