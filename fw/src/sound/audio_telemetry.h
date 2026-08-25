#pragma once
/* Live audio telemetry: the accumulator between the DSP thread and the BLE notifier.
 *
 * Zephyr-aware but deliberately BT-FREE, the same split as
 * audio_dsp_bind_default_bt_dependencies(): sound.cpp must keep zero Bluetooth
 * dependencies, and the GATT wiring lives in fw/src/bluetooth/audio_telemetry_service.cpp.
 *
 * WHY AN ACCUMULATOR AND NOT A QUEUE. Analysis runs at 31.25 Hz; the phone is sent frames
 * at a lower rate (8 Hz by default). A meter wants the FRESHEST state, not a backlog — but
 * beats are events, and dropping the frame a beat landed in would be a lie about the room.
 * So this is level-triggered with one exception: beat flags OR across every frame since the
 * last send. That is exactly the fold the render path already needed for the same reason
 * (issue #376, audio_frame_fold.h), which is why this reuses it rather than writing a
 * second one.
 *
 * THREADING. audio_telemetry_publish() is called from the DSP thread, which has a 2 KB
 * stack, is the sole dmic_read() consumer, and has a hard 32 ms deadline. It therefore does
 * the minimum possible: one atomic check, a spinlock, and a fold. Everything expensive —
 * quantising, packing, and bt_gatt_notify(), which walks connections, allocates a TX buffer
 * and can fail — happens on the system workqueue in the service.
 */

#include <stdbool.h>
#include <stdint.h>

#include "audio_dsp.h"
#include "audio_telemetry_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fold one analysis frame into the pending telemetry snapshot.
 *
 * Cheap and safe to call unconditionally from the DSP thread: returns immediately when no
 * consumer is streaming. Call once per analysis frame, after audio_dsp_process().
 *
 * @param result     the frame just produced
 * @param rms_input_referred smoothed RMS normalised back to 0 dB gain (the gate's comparand)
 * @param rms_instant        this frame's RMS
 * @param peak_norm          this frame's peak, 0..1
 * @param noise_floor        AgcController::noiseFloor()
 * @param gain_steps         current PDM gain relative to the 0 dB park
 * @param frames_since_step  AgcController::framesSinceStep(), saturated by this function
 * @param silent/clipped/agc_frozen  live AGC state
 * @param threshold_mode     0 = mean+alpha*sigma, 1 = median+delta
 * @param alpha              beat alpha, needed to resolve the mode-0 fire line
 * @param beat_flux_floor    the absolute floor, applied to the resolved fire line
 */
void audio_telemetry_publish(const struct audio_analysis_result *result, float rms_input_referred,
                             float rms_instant, float peak_norm, float noise_floor,
                             int8_t gain_steps, uint32_t frames_since_step, bool silent,
                             bool clipped, bool agc_frozen, uint32_t threshold_mode, float alpha,
                             float beat_flux_floor);

/**
 * @brief Take and clear the pending snapshot.
 *
 * @return true if a new frame has arrived since the last take. When false, `out` still holds
 *         the last known state and `dropped` has been incremented — a consumer that sends
 *         anyway is reporting stale data, and the dropped counter is how the app can tell.
 */
bool audio_telemetry_take(struct audio_telemetry_frame *out);

/** @brief Enable/disable accumulation. Level 0 (off) makes publish() a no-op. */
void audio_telemetry_set_active(bool active);

/** @brief Whether accumulation is currently enabled. */
bool audio_telemetry_is_active(void);

/** @brief Drop all pending state (on stream start, so the first frame is not ancient). */
void audio_telemetry_reset(void);

/** @brief Note that a send failed, so the app sees the gap. */
void audio_telemetry_note_send_failure(void);

#ifdef __cplusplus
}
#endif
