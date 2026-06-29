#include "mcuboot_updater.h"

#include <pm_config.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(mcuboot_updater, LOG_LEVEL_INF);

/* ============================================================================
 * Package header format (16 bytes):
 *   [0..3]  magic       = 0x424D5247 ("GRMB" stored little-endian)
 *   [4]     major
 *   [5]     minor
 *   [6..7]  revision    (LE u16)
 *   [8..11] payload_size (LE u32) — bytes of raw MCUboot binary that follow
 *   [12..15] crc32      (LE u32) — IEEE 802.3 CRC32 over payload bytes only
 *   [16..]  raw zephyr.bin
 * ============================================================================ */
static constexpr uint32_t kPkgMagic   = 0x424D5247U;
static constexpr uint32_t kHdrSize    = 16U;
static constexpr uint32_t kPageSize   = 4096U;
static constexpr uint32_t kTotalPages = PM_MCUBOOT_SIZE / kPageSize;

/* Staging file on the FAT filesystem (/NAND: over USB Mass Storage).
 * The BLE upload path writes here directly. Users can also sideload a
 * package by copying it to the USB drive as mcuboot.bin. */
#define MCUBOOT_STAGING_PATH "/NAND:/mcuboot.bin"

struct __attribute__((packed)) McubootPkgHeader {
    uint32_t magic;
    uint8_t  version_major;
    uint8_t  version_minor;
    uint16_t version_revision;
    uint32_t payload_size;
    uint32_t crc32;
};
static_assert(sizeof(McubootPkgHeader) == kHdrSize, "Header size mismatch");
static_assert(PM_MCUBOOT_SIZE % kPageSize == 0, "MCUboot size must be page-aligned");

/* ============================================================================
 * Module state
 * ============================================================================ */
static struct McubootUpdaterStatus s_status = {
    .state          = MCUBOOT_UPDATER_LOCKED,
    .progress       = 0,
    .error          = MCUBOOT_UPDATER_ERR_NONE,
    .flash_unlocked = 0,
};
static mcuboot_updater_status_cb_t s_cb  = NULL;
static uint32_t s_payload_size           = 0;
static uint32_t s_write_offset           = 0; /* bytes written to staging file so far */
static uint8_t  s_prev_progress          = 0;

static K_MUTEX_DEFINE(s_mutex);

static const struct flash_area *s_mcuboot_fa  = NULL; /* internal MCUboot flash region */
static struct fs_file_t s_staging_file;               /* FAT staging file handle */
static bool s_staging_file_open = false;

/* ============================================================================
 * Worker thread
 * ============================================================================ */
K_THREAD_STACK_DEFINE(s_updater_stack, CONFIG_APP_MCUBOOT_UPDATER_STACK_SIZE);
static struct k_work_q      s_updater_wq;
static struct k_work        s_erase_work;
static struct k_work        s_validate_work;
static struct k_work        s_commit_work;
static struct k_work_delayable s_reboot_work;

static void fire_callback(void)
{
    if (s_cb) {
        s_cb(s_status);
    }
}

static void set_error(enum McubootUpdaterError err)
{
    k_mutex_lock(&s_mutex, K_FOREVER);
    s_status.state = MCUBOOT_UPDATER_ERROR;
    s_status.error = err;
    k_mutex_unlock(&s_mutex);
    fire_callback();
}

/* Close the staging file and optionally delete it from the FAT filesystem. */
static void close_staging_file(bool delete_file)
{
    if (s_staging_file_open) {
        fs_close(&s_staging_file);
        s_staging_file_open = false;
    }
    if (delete_file) {
        fs_unlink(MCUBOOT_STAGING_PATH);
    }
}

/* ============================================================================
 * Async work handlers
 * ============================================================================ */
static void reboot_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    LOG_INF("Rebooting into updater mode now...");
    sys_reboot(SYS_REBOOT_WARM);
}

static void erase_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    /* "Erase" for FAT staging: open the file with truncation. Nearly instantaneous
     * compared to erasing a flash partition — the ERASING state is very brief. */
    fs_file_t_init(&s_staging_file);
    int rc = fs_open(&s_staging_file, MCUBOOT_STAGING_PATH,
                     FS_O_CREATE | FS_O_RDWR | FS_O_TRUNC);
    if (rc) {
        LOG_ERR("Failed to open staging file %s: %d", MCUBOOT_STAGING_PATH, rc);
        set_error(MCUBOOT_UPDATER_ERR_STAGING_ERASE);
        return;
    }
    s_staging_file_open = true;

    k_mutex_lock(&s_mutex, K_FOREVER);
    s_status.state    = MCUBOOT_UPDATER_RECEIVING;
    s_status.progress = 0;
    s_write_offset    = 0;
    s_prev_progress   = 0;
    k_mutex_unlock(&s_mutex);

    LOG_INF("Staging file opened, ready to receive %u bytes", s_payload_size);
    fire_callback();
}

static void validate_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    int rc = fs_seek(&s_staging_file, 0, FS_SEEK_SET);
    if (rc) {
        LOG_ERR("Failed to seek staging file: %d", rc);
        close_staging_file(true);
        set_error(MCUBOOT_UPDATER_ERR_STAGING_READ);
        return;
    }

    struct McubootPkgHeader hdr = {};
    rc = fs_read(&s_staging_file, &hdr, sizeof(hdr));
    if (rc != (int)sizeof(hdr)) {
        LOG_ERR("Failed to read staging header: %d", rc);
        close_staging_file(true);
        set_error(MCUBOOT_UPDATER_ERR_STAGING_READ);
        return;
    }

    if (hdr.magic != kPkgMagic) {
        LOG_ERR("Bad magic: 0x%08x (expected 0x%08x)", hdr.magic, kPkgMagic);
        close_staging_file(true);
        set_error(MCUBOOT_UPDATER_ERR_INVALID_MAGIC);
        return;
    }

    if (hdr.payload_size != s_payload_size) {
        LOG_ERR("Header payload_size %u != expected %u", hdr.payload_size, s_payload_size);
        close_staging_file(true);
        set_error(MCUBOOT_UPDATER_ERR_INVALID_SIZE);
        return;
    }

    /* Incrementally compute CRC32 over the payload — file is already positioned
     * after the header from the read above. */
    static uint8_t crc_buf[kPageSize];
    uint32_t running   = 0;
    uint32_t remaining = hdr.payload_size;

    while (remaining > 0) {
        uint32_t chunk = MIN(remaining, sizeof(crc_buf));
        rc = fs_read(&s_staging_file, crc_buf, chunk);
        if (rc != (int)chunk) {
            LOG_ERR("CRC read failed (got %d, expected %u)", rc, chunk);
            close_staging_file(true);
            set_error(MCUBOOT_UPDATER_ERR_STAGING_READ);
            return;
        }
        running = crc32_ieee_update(running, crc_buf, chunk);
        remaining -= chunk;
    }

    if (running != hdr.crc32) {
        LOG_ERR("CRC32 mismatch: computed=0x%08x, header=0x%08x", running, hdr.crc32);
        close_staging_file(true);
        set_error(MCUBOOT_UPDATER_ERR_CRC_MISMATCH);
        return;
    }

    LOG_INF("Validation passed: v%u.%u.%u, %u bytes, CRC32=0x%08x",
            hdr.version_major, hdr.version_minor, hdr.version_revision,
            hdr.payload_size, hdr.crc32);

    /* Leave file open — commit_work_handler will seek and read from it. */
    k_mutex_lock(&s_mutex, K_FOREVER);
    s_status.state    = MCUBOOT_UPDATER_VALIDATED;
    s_status.progress = 100;
    k_mutex_unlock(&s_mutex);
    fire_callback();
}

static void commit_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    /*
     * Write pages in reverse order: pages 1..N-1 first, then page 0 last.
     * Page 0 contains the ARM reset vector table — if power is lost before
     * page 0 is erased, the old MCUboot reset vector is still intact and
     * the device can be recovered. Only the final ~100 ms (page 0 erase +
     * write) is the unrecoverable window that requires J-Link.
     */
    static uint8_t page_buf[kPageSize];

    LOG_INF("Flashing MCUboot: %u pages (%u bytes)", kTotalPages, PM_MCUBOOT_SIZE);

    for (uint32_t p = 1; p < kTotalPages; p++) {
        off_t internal_off = (off_t)(p * kPageSize);
        off_t file_off     = (off_t)(kHdrSize + p * kPageSize);

        int rc = fs_seek(&s_staging_file, file_off, FS_SEEK_SET);
        if (rc) {
            LOG_ERR("Seek failed for page %u: %d", p, rc);
            close_staging_file(false);
            set_error(MCUBOOT_UPDATER_ERR_STAGING_READ);
            return;
        }

        rc = fs_read(&s_staging_file, page_buf, kPageSize);
        if (rc != (int)kPageSize) {
            LOG_ERR("Read failed for page %u: got %d", p, rc);
            close_staging_file(false);
            set_error(MCUBOOT_UPDATER_ERR_STAGING_READ);
            return;
        }

        rc = flash_area_erase(s_mcuboot_fa, internal_off, kPageSize);
        if (rc) {
            LOG_ERR("Internal flash erase failed at page %u: %d", p, rc);
            close_staging_file(false);
            set_error(MCUBOOT_UPDATER_ERR_INTERNAL_ERASE);
            return;
        }

        rc = flash_area_write(s_mcuboot_fa, internal_off, page_buf, kPageSize);
        if (rc) {
            LOG_ERR("Internal flash write failed at page %u: %d", p, rc);
            close_staging_file(false);
            set_error(MCUBOOT_UPDATER_ERR_INTERNAL_WRITE);
            return;
        }

        uint8_t prog = (uint8_t)(((p + 1) * 95) / kTotalPages); /* reserve last 5% for page 0 */
        k_mutex_lock(&s_mutex, K_FOREVER);
        s_status.progress = prog;
        k_mutex_unlock(&s_mutex);
        fire_callback();
    }

    /* Page 0 — point of no return. Power loss here requires J-Link recovery. */
    LOG_INF("Writing page 0 (reset vector) — point of no return");

    int rc = fs_seek(&s_staging_file, (off_t)kHdrSize, FS_SEEK_SET);
    if (rc) {
        LOG_ERR("Seek failed for page 0: %d", rc);
        close_staging_file(false);
        set_error(MCUBOOT_UPDATER_ERR_STAGING_READ);
        return;
    }

    rc = fs_read(&s_staging_file, page_buf, kPageSize);
    if (rc != (int)kPageSize) {
        LOG_ERR("Read failed for page 0: got %d", rc);
        close_staging_file(false);
        set_error(MCUBOOT_UPDATER_ERR_STAGING_READ);
        return;
    }

    rc = flash_area_erase(s_mcuboot_fa, 0, kPageSize);
    if (rc) {
        LOG_ERR("Internal flash erase failed for page 0: %d", rc);
        close_staging_file(false);
        set_error(MCUBOOT_UPDATER_ERR_INTERNAL_ERASE);
        return;
    }

    rc = flash_area_write(s_mcuboot_fa, 0, page_buf, kPageSize);
    if (rc) {
        LOG_ERR("Internal flash write failed for page 0: %d", rc);
        close_staging_file(false);
        set_error(MCUBOOT_UPDATER_ERR_INTERNAL_WRITE);
        return;
    }

    /* File has been fully consumed — delete it to free FAT space. */
    close_staging_file(true);

    k_mutex_lock(&s_mutex, K_FOREVER);
    s_status.state    = MCUBOOT_UPDATER_DONE;
    s_status.progress = 100;
    k_mutex_unlock(&s_mutex);
    fire_callback();

    LOG_INF("MCUboot flashed successfully. Rebooting in 500 ms...");
    k_msleep(500); /* Give the BLE stack time to transmit the DONE notification */
    sys_reboot(SYS_REBOOT_WARM);
}

/* ============================================================================
 * Public API
 * ============================================================================ */
void mcuboot_updater_init(mcuboot_updater_status_cb_t cb)
{
    s_cb = cb;

    int rc = flash_area_open(FIXED_PARTITION_ID(mcuboot), &s_mcuboot_fa);
    if (rc) {
        LOG_ERR("Failed to open mcuboot flash area: %d", rc);
    }

    fs_file_t_init(&s_staging_file);

    /* Check whether MCUboot's fprotect_hook skipped protection this boot.
     * BOOT_MODE_UPDATER_ACTIVE (0xB2) is written by the hook when it grants
     * an updater-mode boot.  We deliberately do NOT clear it here — the hook
     * needs to see 0xB2 (not 0xB1) on the post-commit reboot so it knows to
     * apply fprotect again and restore normal protection. */
    int bm = bootmode_check(MCUBOOT_UPDATER_BOOT_MODE_ACTIVE);
    s_status.flash_unlocked = (bm == 1) ? 1U : 0U;

    k_work_queue_init(&s_updater_wq);
    k_work_queue_start(&s_updater_wq, s_updater_stack,
                       K_THREAD_STACK_SIZEOF(s_updater_stack),
                       CONFIG_NUM_PREEMPT_PRIORITIES - 1, NULL);
    k_work_init(&s_erase_work,    erase_work_handler);
    k_work_init(&s_validate_work, validate_work_handler);
    k_work_init(&s_commit_work,   commit_work_handler);
    k_work_init_delayable(&s_reboot_work, reboot_work_handler);

    LOG_INF("MCUboot updater initialized (state: LOCKED, flash_unlocked=%d, staging: %s)",
            s_status.flash_unlocked, MCUBOOT_STAGING_PATH);
}

int mcuboot_updater_unlock(void)
{
    k_mutex_lock(&s_mutex, K_FOREVER);
    if (s_status.state != MCUBOOT_UPDATER_LOCKED) {
        k_mutex_unlock(&s_mutex);
        return -EBUSY;
    }
    s_status.state = MCUBOOT_UPDATER_IDLE;
    s_status.error = MCUBOOT_UPDATER_ERR_NONE;
    k_mutex_unlock(&s_mutex);

    LOG_INF("MCUboot updater unlocked");
    fire_callback();
    return 0;
}

int mcuboot_updater_begin(uint32_t payload_size)
{
    k_mutex_lock(&s_mutex, K_FOREVER);

    if (s_status.state != MCUBOOT_UPDATER_IDLE) {
        k_mutex_unlock(&s_mutex);
        LOG_ERR("begin() called in wrong state %d", s_status.state);
        return -EBUSY;
    }

    if (payload_size > PM_MCUBOOT_SIZE) {
        k_mutex_unlock(&s_mutex);
        LOG_ERR("payload_size %u exceeds MCUboot partition size %u",
                payload_size, PM_MCUBOOT_SIZE);
        return -EINVAL;
    }

    s_payload_size    = payload_size;
    s_status.state    = MCUBOOT_UPDATER_ERASING;
    s_status.progress = 0;
    s_status.error    = MCUBOOT_UPDATER_ERR_NONE;
    k_mutex_unlock(&s_mutex);

    LOG_INF("Beginning upload of %u bytes, opening staging file...", payload_size);
    fire_callback();
    k_work_submit_to_queue(&s_updater_wq, &s_erase_work);
    return 0;
}

int mcuboot_updater_write_chunk(const uint8_t *data, uint16_t len)
{
    /* Fast state check before taking the mutex — avoids blocking BT RX on mutex */
    if (s_status.state != MCUBOOT_UPDATER_RECEIVING) {
        return -EBUSY;
    }

    k_mutex_lock(&s_mutex, K_FOREVER);

    if (s_write_offset + len > kHdrSize + s_payload_size) {
        k_mutex_unlock(&s_mutex);
        LOG_ERR("Chunk overflow: write_offset=%u + len=%u > hdr+payload=%u",
                s_write_offset, len, kHdrSize + s_payload_size);
        return -EOVERFLOW;
    }

    /* FAT writes need no alignment padding (unlike raw QSPI NOR). */
    int rc = fs_write(&s_staging_file, data, len);
    if (rc < 0 || (uint16_t)rc != len) {
        s_status.state = MCUBOOT_UPDATER_ERROR;
        s_status.error = MCUBOOT_UPDATER_ERR_STAGING_WRITE;
        k_mutex_unlock(&s_mutex);
        LOG_ERR("Staging write failed at offset %u: %d", s_write_offset, rc);
        fire_callback();
        return (rc < 0) ? rc : -EIO;
    }

    s_write_offset += len;

    uint32_t total = kHdrSize + s_payload_size;
    uint8_t  prog  = (uint8_t)((s_write_offset * 100U) / total);
    s_status.progress = prog;

    k_mutex_unlock(&s_mutex);

    /* Fire callback on meaningful progress increments to avoid notification spam */
    if (prog >= s_prev_progress + 5 || s_write_offset == total) {
        s_prev_progress = prog;
        fire_callback();
    }

    return 0;
}

int mcuboot_updater_validate(void)
{
    k_mutex_lock(&s_mutex, K_FOREVER);

    if (s_status.state != MCUBOOT_UPDATER_RECEIVING) {
        k_mutex_unlock(&s_mutex);
        LOG_ERR("validate() called in wrong state %d", s_status.state);
        return -EBUSY;
    }

    if (s_write_offset != kHdrSize + s_payload_size) {
        k_mutex_unlock(&s_mutex);
        LOG_ERR("validate() called with incomplete data: %u of %u bytes received",
                s_write_offset, kHdrSize + s_payload_size);
        return -EINVAL;
    }

    s_status.state    = MCUBOOT_UPDATER_VALIDATING;
    s_status.progress = 0;
    k_mutex_unlock(&s_mutex);

    LOG_INF("Validating staged binary...");
    fire_callback();
    k_work_submit_to_queue(&s_updater_wq, &s_validate_work);
    return 0;
}

int mcuboot_updater_commit(void)
{
    k_mutex_lock(&s_mutex, K_FOREVER);

    if (s_status.state != MCUBOOT_UPDATER_VALIDATED) {
        k_mutex_unlock(&s_mutex);
        LOG_ERR("commit() called in wrong state %d", s_status.state);
        return -EBUSY;
    }

    s_status.state    = MCUBOOT_UPDATER_FLASHING;
    s_status.progress = 0;
    k_mutex_unlock(&s_mutex);

    LOG_INF("Committing MCUboot update — flashing internal flash...");
    fire_callback();
    k_work_submit_to_queue(&s_updater_wq, &s_commit_work);
    return 0;
}

void mcuboot_updater_abort(void)
{
    close_staging_file(true);

    k_mutex_lock(&s_mutex, K_FOREVER);
    s_status.state    = MCUBOOT_UPDATER_LOCKED;
    s_status.progress = 0;
    s_status.error    = MCUBOOT_UPDATER_ERR_NONE;
    s_payload_size    = 0;
    s_write_offset    = 0;
    k_mutex_unlock(&s_mutex);

    LOG_INF("MCUboot updater aborted, returning to LOCKED");
    fire_callback();
}

struct McubootUpdaterStatus mcuboot_updater_get_status(void)
{
    k_mutex_lock(&s_mutex, K_FOREVER);
    struct McubootUpdaterStatus st = s_status;
    k_mutex_unlock(&s_mutex);
    return st;
}

int mcuboot_updater_request_updater_reboot(void)
{
    int rc = bootmode_set(MCUBOOT_UPDATER_BOOT_MODE_REQ);
    if (rc) {
        LOG_ERR("Failed to set updater boot mode: %d", rc);
        return rc;
    }
    LOG_INF("Updater reboot requested; rebooting in 200 ms");
    /* Delay gives the BLE ATT response time to be transmitted before the
     * radio disappears with the reboot. */
    k_work_schedule_for_queue(&s_updater_wq, &s_reboot_work, K_MSEC(200));
    return 0;
}

/* ============================================================================
 * Shell commands (for development / testing)
 * ============================================================================ */
static int cmd_mcuboot_update_status(const struct shell *sh, size_t argc, char **argv)
{
    struct McubootUpdaterStatus st = mcuboot_updater_get_status();
    static const char *const state_names[] = {
        "LOCKED", "IDLE", "ERASING", "RECEIVING", "VALIDATING",
        "VALIDATED", "FLASHING", "DONE", "ERROR",
    };
    const char *state_str = (st.state < ARRAY_SIZE(state_names))
                            ? state_names[st.state] : "UNKNOWN";
    shell_print(sh, "state=%s progress=%u error=%d flash_unlocked=%u",
                state_str, st.progress, st.error, st.flash_unlocked);
    return 0;
}

static int cmd_mcuboot_update_abort(const struct shell *sh, size_t argc, char **argv)
{
    mcuboot_updater_abort();
    shell_print(sh, "Aborted, state is now LOCKED");
    return 0;
}

static int cmd_mcuboot_update_request_reboot(const struct shell *sh, size_t argc, char **argv)
{
    int rc = mcuboot_updater_request_updater_reboot();
    if (rc) {
        shell_error(sh, "Failed to request updater reboot: %d", rc);
        return rc;
    }
    shell_print(sh, "Updater reboot requested — device will reboot in ~200 ms");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(mcuboot_update_cmds,
    SHELL_CMD(status,          NULL, "Print updater state",                  cmd_mcuboot_update_status),
    SHELL_CMD(abort,           NULL, "Abort and lock",                       cmd_mcuboot_update_abort),
    SHELL_CMD(request_reboot,  NULL, "Set UPDATER_REQ boot mode and reboot", cmd_mcuboot_update_request_reboot),
    SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(mcuboot_update, &mcuboot_update_cmds,
                   "MCUboot BLE updater commands", NULL);
