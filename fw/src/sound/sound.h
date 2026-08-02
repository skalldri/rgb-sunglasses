#pragma once
#include <zephyr/kernel.h>

#include <cstdint>

/* AgcConfigProvider moved to agc_controller.h (issue #264 Phase 2) so the
 * Zephyr-free AgcController and its native_sim/replay consumers can see it
 * without dragging in this header's kernel dependencies. The seam rationale
 * (decoupling sound.cpp from the BT/Settings-backed implementation, with both
 * getters and setters for the "sound agc" shell commands) is documented there. */
#include "agc_controller.h"
#include "audio_dsp.h"

extern struct k_msgq audio_result_q;

/**
 * @brief Sets the provider the AGC loop and shell commands read/write through.
 *
 * Pass nullptr to revert to the built-in default (historical static-variable values).
 */
void sound_set_agc_config_provider(AgcConfigProvider *provider);
