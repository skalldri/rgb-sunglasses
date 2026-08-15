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

/* Capture space budget — the ONE definition of what a capture costs.
 *
 * capture.cpp turns free space into the recordable-seconds figure the app
 * shows and the clamp capture_start() applies; record_wav_capture()'s
 * pre-flight then re-checks the clamped length. If the two disagree and this
 * side is the more generous, the clamp advertises lengths the pre-flight
 * rejects with -ENOSPC — which is not an edge case but every capture at the
 * advertised maximum. They were literals in two files agreeing by hand until
 * exactly that shipped; both now derive from here, and BUILD_ASSERTs in
 * sound.cpp tie these to the values they mirror. */
#define CAPTURE_BLOCK_TIME_MS 32u
#define CAPTURE_WAV_BYTES_PER_FRAME 1024u
#define CAPTURE_IMU_BYTES_PER_FRAME 56u      /* 25 Hz of I-rows, charged per audio frame */
#define CAPTURE_ANALYSIS_BYTES_PER_FRAME 360u /* one 41-field D-line per block */
#define CAPTURE_OVERHEAD_SLACK_BYTES (64u * 1024u)

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
