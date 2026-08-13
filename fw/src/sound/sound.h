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

/**
 * @brief Run a capture synchronously, dispatching to the tap or direct path.
 *
 * Extracted from the shell command so the background capture worker
 * (src/sound/capture.cpp) can drive the same loop: a GATT write cannot block on
 * BT RX for the length of a recording, but the loop itself is identical whoever
 * asks for it.
 *
 * @param shell Where progress and errors go. May be the UART shell obtained by a
 *        non-shell caller; must not be NULL.
 * @param duration_s Hard cap in seconds.
 * @param path Output WAV path (the .imu.csv sidecar sits beside it).
 * @return 0 on success — INCLUDING a capture stopped early by
 *         sound_record_request_stop() — or a negative errno.
 */
int sound_record_wav(const struct shell *shell, uint32_t duration_s, const char *path);

/**
 * @brief Ask a running capture to finish early and finalise its files.
 *
 * A requested stop is a SUCCESS, not the existing io_error abort: the recording
 * simply ended when the user said so, and the WAV and sidecar are closed and
 * valid. Cleared by sound_record_arm(), not by sound_record_wav().
 */
void sound_record_request_stop(void);

/**
 * @brief Clear any pending stop request, at the moment a capture is REQUESTED.
 *
 * Separate from sound_record_wav() so that a stop arriving during the pre-roll —
 * after the capture manager has published RECORDING and lit the app's Stop
 * button, but before the worker reaches the loop — is still honoured instead of
 * being cleared out from under the user.
 */
void sound_record_arm(void);
