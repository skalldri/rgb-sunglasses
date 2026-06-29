#include "mcuboot_updater.h"

#include <pm_config.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
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
    .state    = MCUBOOT_UPDATER_LOCKED,
    .progress = 0,
    .error    = MCUBOOT_UPDATER_ERR_NONE,
};
static mcuboot_updater_status_cb_t s_cb  = NULL;
static uint32_t s_payload_size           = 0;
static uint32_t s_write_offset           = 0; /* bytes written to staging so far */
static uint8_t  s_prev_progress          = 0;

static K_MUTEX_DEFINE(s_mutex);

static const struct flash_area *s_mcuboot_fa  = NULL;
static const struct flash_area *s_staging_fa  = NULL;

/* ============================================================================
 * Worker thread
 * ============================================================================ */
K_THREAD_STACK_DEFINE(s_updater_stack, CONFIG_APP_MCUBOOT_UPDATER_STACK_SIZE);
static struct k_work_q s_updater_wq;
static struct k_work   s_erase_work;
static struct k_work   s_validate_work;
static struct k_work   s_commit_work;

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

/* ============================================================================
 * Async work handlers
 * ============================================================================ */
static void erase_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    LOG_INF("Erasing staging partition (%u bytes)...", PM_MCUBOOT_STAGING_SIZE);
    int rc = flash_area_erase(s_staging_fa, 0, PM_MCUBOOT_STAGING_SIZE);
    if (rc) {
        LOG_ERR("Staging erase failed: %d", rc);
        set_error(MCUBOOT_UPDATER_ERR_STAGING_ERASE);
        return;
    }

    k_mutex_lock(&s_mutex, K_FOREVER);
    s_status.state    = MCUBOOT_UPDATER_RECEIVING;
    s_status.progress = 0;
    s_write_offset    = 0;
    s_prev_progress   = 0;
    k_mutex_unlock(&s_mutex);

    LOG_INF("Staging partition erased, ready to receive %u bytes", s_payload_size);
    fire_callback();
}

static void validate_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    /* Read and validate the package header from staging flash */
    struct McubootPkgHeader hdr = {};
    int rc = flash_area_read(s_staging_fa, 0, &hdr, sizeof(hdr));
    if (rc) {
        LOG_ERR("Failed to read staging header: %d", rc);
        set_error(MCUBOOT_UPDATER_ERR_STAGING_READ);
        return;
    }

    if (hdr.magic != kPkgMagic) {
        LOG_ERR("Bad magic: 0x%08x (expected 0x%08x)", hdr.magic, kPkgMagic);
        set_error(MCUBOOT_UPDATER_ERR_INVALID_MAGIC);
        return;
    }

    if (hdr.payload_size != s_payload_size) {
        LOG_ERR("Header payload_size %u != expected %u", hdr.payload_size, s_payload_size);
        set_error(MCUBOOT_UPDATER_ERR_INVALID_SIZE);
        return;
    }

    /* Incrementally compute CRC32 over the payload in 4 KB chunks */
    static uint8_t crc_buf[kPageSize];
    uint32_t running = 0;
    uint32_t remaining = hdr.payload_size;
    off_t off = kHdrSize;

    while (remaining > 0) {
        uint32_t chunk = MIN(remaining, sizeof(crc_buf));
        rc = flash_area_read(s_staging_fa, off, crc_buf, chunk);
        if (rc) {
            LOG_ERR("CRC read failed at offset %zu: %d", (size_t)off, rc);
            set_error(MCUBOOT_UPDATER_ERR_STAGING_READ);
            return;
        }
        running = crc32_ieee_update(running, crc_buf, chunk);
        off       += chunk;
        remaining -= chunk;
    }

    if (running != hdr.crc32) {
        LOG_ERR("CRC32 mismatch: computed=0x%08x, header=0x%08x", running, hdr.crc32);
        set_error(MCUBOOT_UPDATER_ERR_CRC_MISMATCH);
        return;
    }

    LOG_INF("Validation passed: v%u.%u.%u, %u bytes, CRC32=0x%08x",
            hdr.version_major, hdr.version_minor, hdr.version_revision,
            hdr.payload_size, hdr.crc32);

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
        off_t staging_off  = (off_t)(kHdrSize + p * kPageSize);

        int rc = flash_area_read(s_staging_fa, staging_off, page_buf, kPageSize);
        if (rc) {
            LOG_ERR("Staging read failed at page %u: %d", p, rc);
            set_error(MCUBOOT_UPDATER_ERR_STAGING_READ);
            return;
        }

        rc = flash_area_erase(s_mcuboot_fa, internal_off, kPageSize);
        if (rc) {
            LOG_ERR("Internal flash erase failed at page %u: %d", p, rc);
            set_error(MCUBOOT_UPDATER_ERR_INTERNAL_ERASE);
            return;
        }

        rc = flash_area_write(s_mcuboot_fa, internal_off, page_buf, kPageSize);
        if (rc) {
            LOG_ERR("Internal flash write failed at page %u: %d", p, rc);
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
    int rc = flash_area_read(s_staging_fa, (off_t)kHdrSize, page_buf, kPageSize);
    if (rc) {
        LOG_ERR("Staging read failed for page 0: %d", rc);
        set_error(MCUBOOT_UPDATER_ERR_STAGING_READ);
        return;
    }

    rc = flash_area_erase(s_mcuboot_fa, 0, kPageSize);
    if (rc) {
        LOG_ERR("Internal flash erase failed for page 0: %d", rc);
        set_error(MCUBOOT_UPDATER_ERR_INTERNAL_ERASE);
        return;
    }

    rc = flash_area_write(s_mcuboot_fa, 0, page_buf, kPageSize);
    if (rc) {
        LOG_ERR("Internal flash write failed for page 0: %d", rc);
        set_error(MCUBOOT_UPDATER_ERR_INTERNAL_WRITE);
        return;
    }

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

    rc = flash_area_open(FIXED_PARTITION_ID(mcuboot_staging), &s_staging_fa);
    if (rc) {
        LOG_ERR("Failed to open mcuboot_staging flash area: %d", rc);
    }

    k_work_queue_init(&s_updater_wq);
    k_work_queue_start(&s_updater_wq, s_updater_stack,
                       K_THREAD_STACK_SIZEOF(s_updater_stack),
                       CONFIG_NUM_PREEMPT_PRIORITIES - 1, NULL);
    k_work_init(&s_erase_work,    erase_work_handler);
    k_work_init(&s_validate_work, validate_work_handler);
    k_work_init(&s_commit_work,   commit_work_handler);

    LOG_INF("MCUboot updater initialized (state: LOCKED)");
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

    if (payload_size > (PM_MCUBOOT_STAGING_SIZE - kHdrSize)) {
        k_mutex_unlock(&s_mutex);
        LOG_ERR("payload_size %u exceeds staging capacity %u", payload_size,
                PM_MCUBOOT_STAGING_SIZE - kHdrSize);
        return -EINVAL;
    }

    if (payload_size > PM_MCUBOOT_SIZE) {
        k_mutex_unlock(&s_mutex);
        LOG_ERR("payload_size %u exceeds MCUboot partition size %u", payload_size,
                PM_MCUBOOT_SIZE);
        return -EINVAL;
    }

    s_payload_size    = payload_size;
    s_status.state    = MCUBOOT_UPDATER_ERASING;
    s_status.progress = 0;
    s_status.error    = MCUBOOT_UPDATER_ERR_NONE;
    k_mutex_unlock(&s_mutex);

    LOG_INF("Beginning upload of %u bytes, erasing staging partition...", payload_size);
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

    /*
     * Pad final chunk to 4-byte boundary for QSPI write alignment.
     * The CRC check uses header.payload_size bytes, not the padded amount,
     * so 0xFF padding bytes at the end are harmless.
     */
    uint16_t padded_len = len;
    static uint8_t pad_buf[244 + 3]; /* max chunk + up to 3 pad bytes */
    if (len % 4 != 0) {
        memcpy(pad_buf, data, len);
        while (padded_len % 4 != 0) {
            pad_buf[padded_len++] = 0xFF;
        }
        data = pad_buf;
    }

    int rc = flash_area_write(s_staging_fa, (off_t)s_write_offset, data, padded_len);
    if (rc) {
        s_status.state = MCUBOOT_UPDATER_ERROR;
        s_status.error = MCUBOOT_UPDATER_ERR_STAGING_WRITE;
        k_mutex_unlock(&s_mutex);
        LOG_ERR("Staging write failed at offset %u: %d", s_write_offset, rc);
        fire_callback();
        return rc;
    }

    s_write_offset += len; /* advance by unpadded length */

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
    shell_print(sh, "state=%s progress=%u error=%d", state_str, st.progress, st.error);
    return 0;
}

static int cmd_mcuboot_update_abort(const struct shell *sh, size_t argc, char **argv)
{
    mcuboot_updater_abort();
    shell_print(sh, "Aborted, state is now LOCKED");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(mcuboot_update_cmds,
    SHELL_CMD(status, NULL, "Print updater state", cmd_mcuboot_update_status),
    SHELL_CMD(abort,  NULL, "Abort and lock",      cmd_mcuboot_update_abort),
    SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(mcuboot_update, &mcuboot_update_cmds,
                   "MCUboot BLE updater commands", NULL);
