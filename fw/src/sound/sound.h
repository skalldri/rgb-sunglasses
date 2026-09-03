#pragma once

#include <zephyr/sys/util.h>
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
/* record_wav_tap()'s D-line allowance. Its pre-flight charges BLOCK_SIZE + 175
 * per frame, NOT the 360 above: it writes a 21-field line (no display buckets).
 * Kept here rather than in capture.cpp because on a CONFIG_APP_AUDIO_DEBUG
 * build sound_record_wav() routes an app-started capture to THAT function, so
 * this is the cost the recordable-seconds clamp has to be measured against. */
#define CAPTURE_TAP_ANALYSIS_BYTES_PER_FRAME 175u
#define CAPTURE_OVERHEAD_SLACK_BYTES (64u * 1024u)
#define CAPTURE_SECTOR_BYTES 4096u
/* Fixed cost: FAT slack, the sector-padded WAV prologue, and the CSV's
 * sector-padded header when any CSV is written. Both the clamp and the
 * pre-flight use THIS — leaving the reserve as two literals is what left the
 * half that actually differed between the files unchecked. */
#define CAPTURE_OVERHEAD_BYTES                                                  \
    (CAPTURE_OVERHEAD_SLACK_BYTES + CAPTURE_SECTOR_BYTES +                      \
     ((IS_ENABLED(CONFIG_IMU) || IS_ENABLED(CONFIG_APP_CAPTURE_AUDIO_SIDECAR))  \
          ? CAPTURE_SECTOR_BYTES                                                \
          : 0u))

/* One analysis frame's capture window (ms) and audio_result_q's depth. Exported
 * because the FFT bars proration depends on both numerically (its withdrawal
 * rate assumes this cadence; its catch-up cap mirrors this depth), and a silent
 * drift would revert the fast-render path to the pre-round-6 burst behavior
 * with no test failing — the audio adapter BUILD_ASSERTs them against the
 * animation's mirrors (PR #378 review round 9). sound.cpp derives its capture
 * block size and the msgq definition from these same macros. */
#define BLOCK_CAPTURE_TIME_MS 32
#define AUDIO_RESULT_QUEUE_DEPTH 4

extern struct k_msgq audio_result_q;

/**
 * @brief Sets the provider the AGC loop and shell commands read/write through.
 *
 * Pass nullptr to revert to the built-in default (historical static-variable values).
 */
void sound_set_agc_config_provider(AgcConfigProvider *provider);

/**
 * @brief Injects the FFT Bars display window for the `sound dsp params` diagnostic.
 *
 * Interface-only (animations/fft_visualization_config_source.h): the sound layer prints the
 * window through this pointer and never includes the concrete animation. Bound by
 * audio_dsp_bind_default_bt_dependencies() alongside the other providers; nullptr (the
 * default) simply omits the line.
 */
class FftVisualizationConfigSource;
void sound_set_fft_visualization_source(const FftVisualizationConfigSource *source);

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
