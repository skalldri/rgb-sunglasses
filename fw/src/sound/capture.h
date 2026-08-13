#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @file
 * @brief Background audio + IMU capture, drivable from the shell or over BLE.
 *
 * `sound mic record_wav` runs its capture loop synchronously on the caller's
 * thread, which is fine for a shell command and impossible for a GATT write: a
 * write handler runs on the BT RX thread, and blocking that for the length of a
 * recording would stall the entire Bluetooth stack. So the loop moves behind a
 * worker thread and both front ends queue work to it.
 *
 * Captures are for FIELD use — start one, do the thing, come back. They
 * accumulate on the volume under auto-indexed names and are collected later over
 * USB mass storage; nothing here transfers files, and the MCUmgr filesystem fence
 * (which exists to stop a bonded peer reading /NAND:/mcuboot.bin) is deliberately
 * left alone.
 *
 * BT-free on purpose: the GATT surface lives in
 * src/bluetooth/capture_service.cpp, matching how audio_config.cpp fronts the DSP
 * tunables.
 */

enum capture_state {
    CAPTURE_IDLE = 0,
    CAPTURE_RECORDING = 1,
    CAPTURE_FAILED = 2, /**< last attempt failed; cleared by the next start */
};

/** @brief Snapshot of the capture manager, for the shell and the GATT reads. */
struct capture_status {
    enum capture_state state;
    uint32_t elapsed_s;   /**< into the current capture; 0 when idle */
    uint32_t limit_s;     /**< cap the running capture will stop itself at */
    uint32_t captures;    /**< capture files currently on the volume */
    uint32_t remaining_s; /**< recordable seconds left, from free space */
    int last_error;       /**< errno from the last failed capture, else 0 */
};

/**
 * @brief Begin a capture, returning immediately.
 *
 * @param limit_s Hard stop after this many seconds. A cap is mandatory rather
 *        than optional: an unattended capture that nobody stops fills the volume,
 *        and the volume only holds ~3 minutes of audio in the first place, so the
 *        cap makes an existing physical limit explicit instead of adding one.
 *        Clamped to what the free space can actually hold.
 * @return 0 on success, -EBUSY if one is already running, -ENOSPC if the volume
 *         cannot hold a useful capture, or another negative errno.
 */
int capture_start(uint32_t limit_s);

/**
 * @brief Ask the running capture to stop early and finalise its files.
 * @return 0 if a capture was running, -EALREADY if not.
 */
int capture_stop(void);

/** @brief Current state, safe to call from any thread. */
void capture_get_status(struct capture_status *out);

/** @brief Recordable seconds the free space can hold, for UI and pre-flight. */
uint32_t capture_remaining_seconds(void);

/**
 * @brief Seconds into the running capture, 0 when idle.
 *
 * Deliberately cheaper than capture_get_status(): it touches only the state
 * mutex, no filesystem. That is what makes it safe to call once a second from a
 * workqueue to drive a live progress readout — capture_get_status() walks the
 * volume for the file count, which is flash I/O and must not run on a
 * cooperative-priority workqueue thread.
 */
uint32_t capture_elapsed_seconds(void);

/** @brief Whether a capture is currently running (same cheap path as above). */
bool capture_is_recording(void);

/** @brief Count of capture files on the volume. */
uint32_t capture_count(void);

/**
 * @brief Registered by the capture manager; lets the BLE layer push state
 * changes without the manager depending on Bluetooth.
 */
typedef void (*capture_state_observer)(const struct capture_status *status);
void capture_register_observer(capture_state_observer observer);
