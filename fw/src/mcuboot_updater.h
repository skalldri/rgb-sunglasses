#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * State machine for the BLE MCUboot bootloader updater.
 *
 * Default state after init is LOCKED. The state machine refuses all commands
 * until an explicit UNLOCK control command is received, providing a software-level
 * gate on top of the BLE L4 security requirement.
 *
 * Transitions:
 *   LOCKED → IDLE        (via mcuboot_updater_unlock)
 *   IDLE   → ERASING     (via mcuboot_updater_begin)
 *   ERASING→ RECEIVING   (async: erase complete)
 *   RECEIVING → VALIDATING (via mcuboot_updater_validate, all chunks received)
 *   VALIDATING → VALIDATED (async: CRC check passed)
 *   VALIDATED → FLASHING  (via mcuboot_updater_commit)
 *   FLASHING → DONE       (async: all pages written; sys_reboot follows)
 *   Any state → ERROR     (on failure)
 *   Any state → LOCKED    (via mcuboot_updater_abort; also resets to LOCKED, not IDLE)
 */
enum McubootUpdaterState {
    MCUBOOT_UPDATER_LOCKED     = 0,
    MCUBOOT_UPDATER_IDLE       = 1,
    MCUBOOT_UPDATER_ERASING    = 2,
    MCUBOOT_UPDATER_RECEIVING  = 3,
    MCUBOOT_UPDATER_VALIDATING = 4,
    MCUBOOT_UPDATER_VALIDATED  = 5,
    MCUBOOT_UPDATER_FLASHING   = 6,
    MCUBOOT_UPDATER_DONE       = 7,
    MCUBOOT_UPDATER_ERROR      = 8,
};

enum McubootUpdaterError {
    MCUBOOT_UPDATER_ERR_NONE              = 0,
    MCUBOOT_UPDATER_ERR_WRONG_STATE       = 1,
    MCUBOOT_UPDATER_ERR_SIZE_TOO_LARGE    = 2,
    MCUBOOT_UPDATER_ERR_STAGING_ERASE     = 3,
    MCUBOOT_UPDATER_ERR_STAGING_WRITE     = 4,
    MCUBOOT_UPDATER_ERR_INVALID_MAGIC     = 5,
    MCUBOOT_UPDATER_ERR_INVALID_SIZE      = 6,
    MCUBOOT_UPDATER_ERR_CRC_MISMATCH      = 7,
    MCUBOOT_UPDATER_ERR_INTERNAL_ERASE    = 8,
    MCUBOOT_UPDATER_ERR_INTERNAL_WRITE    = 9,
    MCUBOOT_UPDATER_ERR_OVERFLOW          = 10,
    MCUBOOT_UPDATER_ERR_STAGING_READ      = 11,
};

/* Boot-mode values written to the gpregret2 retention register.
 * Must match the #defines in fw/mcuboot_hooks/fprotect_hook.c. */
#define MCUBOOT_UPDATER_BOOT_MODE_REQ    0xB1U  /* app → MCUboot: skip fprotect next boot */
#define MCUBOOT_UPDATER_BOOT_MODE_ACTIVE 0xB2U  /* MCUboot → app: flash region is unlocked */

struct McubootUpdaterStatus {
    enum McubootUpdaterState state;
    uint8_t progress;        /* 0-100 */
    enum McubootUpdaterError error;
    uint8_t flash_unlocked;  /* 1 if MCUboot skipped fprotect this boot, 0 otherwise */
};

typedef void (*mcuboot_updater_status_cb_t)(struct McubootUpdaterStatus status);

/** Register a callback and open flash areas. Called from SYS_INIT. */
void mcuboot_updater_init(mcuboot_updater_status_cb_t cb);

/** Unlock the state machine (LOCKED → IDLE). */
int mcuboot_updater_unlock(void);

/** Begin an upload: validates size and queues an async open of the FAT staging file. */
int mcuboot_updater_begin(uint32_t payload_size);

/**
 * Write a chunk of the package binary to the staging partition.
 * Called directly from the BT RX thread; QSPI writes of ≤244 bytes are fast
 * enough (~1-2 ms) to run synchronously without blocking the BT stack.
 */
int mcuboot_updater_write_chunk(const uint8_t *data, uint16_t len);

/** Trigger async CRC32 validation of the fully-received staging data. */
int mcuboot_updater_validate(void);

/**
 * Trigger async page-by-page erase+write of the internal MCUboot flash region
 * (0x0-0x14000) from the staging partition, then reboot.
 * Irreversible once started — page 0 (reset vector) is written last to maximise
 * the recovery window if power is lost mid-operation.
 */
int mcuboot_updater_commit(void);

/** Abort any in-progress operation and return to LOCKED state. */
void mcuboot_updater_abort(void);

/** Read the current status (safe to call from any thread). */
struct McubootUpdaterStatus mcuboot_updater_get_status(void);

/**
 * Request an updater-mode reboot.
 * Sets the gpregret2 boot-mode to MCUBOOT_UPDATER_BOOT_MODE_REQ then triggers
 * a warm reboot after 200 ms (allowing the BLE ATT response to be sent first).
 * After the reboot, MCUboot's fprotect_hook skips flash protection and sets
 * BOOT_MODE_UPDATER_ACTIVE so the app can confirm the region is writable.
 */
int mcuboot_updater_request_updater_reboot(void);

#ifdef __cplusplus
}
#endif
