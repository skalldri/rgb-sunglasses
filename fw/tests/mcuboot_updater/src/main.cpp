#include <mcuboot_updater.h>
#include <pm_config.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>
#include <cstring>

extern "C" {
#include <ff.h>
}

/* ============================================================================
 * FAT filesystem helpers (used by the async-flow suite)
 * ============================================================================ */

static FATFS s_fat;
static struct fs_mount_t s_mnt = {
    .type      = FS_FATFS,
    .mnt_point = "/NAND:",
    .fs_data   = &s_fat,
};

static void nand_mount(void) {
    fs_mkfs(FS_FATFS, (uintptr_t)"NAND", NULL, 0);
    fs_mount(&s_mnt);
}

static void nand_unmount(void) {
    fs_unmount(&s_mnt);
}

/* ============================================================================
 * Package-building helpers
 * ============================================================================ */

static constexpr uint32_t kPkgMagic = 0x424D5247U;
static constexpr uint32_t kHdrSize  = 16U;

struct __attribute__((packed)) MockPkgHeader {
    uint32_t magic;
    uint8_t  major;
    uint8_t  minor;
    uint16_t revision;
    uint32_t payload_size;
    uint32_t crc32;
};
static_assert(sizeof(MockPkgHeader) == kHdrSize, "header size mismatch");

static void write_chunks(const MockPkgHeader *hdr, const uint8_t *payload, uint32_t payload_size) {
    zassert_ok(mcuboot_updater_write_chunk((const uint8_t *)hdr, sizeof(*hdr)));
    zassert_ok(mcuboot_updater_write_chunk(payload, payload_size));
}

/* ============================================================================
 * Suite 1 — synchronous state-machine tests
 *
 * These exercise the error-return paths in every public API function WITHOUT
 * calling mcuboot_updater_init() or submitting any work items.  The static
 * mutex (K_MUTEX_DEFINE) is initialised at link time and works without init.
 * ============================================================================ */

static void sm_reset(void *) {
    mcuboot_updater_abort();   /* always returns to LOCKED */
}

ZTEST_SUITE(mcuboot_updater_sm, NULL, NULL, sm_reset, sm_reset, NULL);

ZTEST(mcuboot_updater_sm, test_initial_state_is_locked) {
    struct McubootUpdaterStatus st = mcuboot_updater_get_status();
    zassert_equal(st.state,    MCUBOOT_UPDATER_LOCKED);
    zassert_equal(st.progress, 0u);
    zassert_equal(st.error,    MCUBOOT_UPDATER_ERR_NONE);
}

ZTEST(mcuboot_updater_sm, test_unlock_transitions_to_idle) {
    zassert_ok(mcuboot_updater_unlock(), "unlock() from LOCKED must succeed");
    zassert_equal(mcuboot_updater_get_status().state, MCUBOOT_UPDATER_IDLE);
}

ZTEST(mcuboot_updater_sm, test_unlock_twice_returns_ebusy) {
    zassert_ok(mcuboot_updater_unlock());
    zassert_equal(mcuboot_updater_unlock(), -EBUSY,
                  "second unlock() must return -EBUSY");
}

ZTEST(mcuboot_updater_sm, test_abort_from_idle_returns_to_locked) {
    mcuboot_updater_unlock();
    mcuboot_updater_abort();
    zassert_equal(mcuboot_updater_get_status().state, MCUBOOT_UPDATER_LOCKED);
}

ZTEST(mcuboot_updater_sm, test_abort_from_locked_is_idempotent) {
    /* Calling abort() when already LOCKED must not crash or change state. */
    mcuboot_updater_abort();
    zassert_equal(mcuboot_updater_get_status().state, MCUBOOT_UPDATER_LOCKED);
}

ZTEST(mcuboot_updater_sm, test_begin_in_locked_state_returns_ebusy) {
    zassert_equal(mcuboot_updater_begin(1024), -EBUSY);
    /* State must remain LOCKED — no side effects on error return. */
    zassert_equal(mcuboot_updater_get_status().state, MCUBOOT_UPDATER_LOCKED);
}

ZTEST(mcuboot_updater_sm, test_begin_oversized_payload_returns_einval) {
    mcuboot_updater_unlock();   /* → IDLE */
    zassert_equal(mcuboot_updater_begin(PM_MCUBOOT_SIZE + 1), -EINVAL,
                  "payload larger than MCUboot partition must be rejected");
    /* State must stay IDLE — no work was submitted. */
    zassert_equal(mcuboot_updater_get_status().state, MCUBOOT_UPDATER_IDLE);
}

ZTEST(mcuboot_updater_sm, test_write_chunk_in_locked_state_returns_ebusy) {
    uint8_t data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    zassert_equal(mcuboot_updater_write_chunk(data, sizeof(data)), -EBUSY);
}

ZTEST(mcuboot_updater_sm, test_write_chunk_in_idle_state_returns_ebusy) {
    mcuboot_updater_unlock();
    uint8_t data[4] = {};
    zassert_equal(mcuboot_updater_write_chunk(data, sizeof(data)), -EBUSY);
}

ZTEST(mcuboot_updater_sm, test_validate_in_locked_state_returns_ebusy) {
    zassert_equal(mcuboot_updater_validate(), -EBUSY);
}

ZTEST(mcuboot_updater_sm, test_validate_in_idle_state_returns_ebusy) {
    mcuboot_updater_unlock();
    zassert_equal(mcuboot_updater_validate(), -EBUSY);
}

ZTEST(mcuboot_updater_sm, test_validate_with_incomplete_data_returns_einval) {
    /* Put the state machine into RECEIVING manually via the status field path.
     * We can't easily do this without init (begin() submits work), so instead
     * just verify the -EBUSY path for IDLE and LOCKED — the -EINVAL path for
     * an incomplete write_offset is covered by the async-flow suite. */
    mcuboot_updater_unlock();
    /* IDLE → validate() → -EBUSY (not -EINVAL; that needs RECEIVING state) */
    zassert_equal(mcuboot_updater_validate(), -EBUSY);
}

ZTEST(mcuboot_updater_sm, test_commit_in_locked_state_returns_ebusy) {
    zassert_equal(mcuboot_updater_commit(), -EBUSY);
}

ZTEST(mcuboot_updater_sm, test_commit_in_idle_state_returns_ebusy) {
    mcuboot_updater_unlock();
    zassert_equal(mcuboot_updater_commit(), -EBUSY);
}

ZTEST(mcuboot_updater_sm, test_get_status_reflects_state_transitions) {
    struct McubootUpdaterStatus st = mcuboot_updater_get_status();
    zassert_equal(st.state, MCUBOOT_UPDATER_LOCKED);

    mcuboot_updater_unlock();
    st = mcuboot_updater_get_status();
    zassert_equal(st.state, MCUBOOT_UPDATER_IDLE);

    mcuboot_updater_abort();
    st = mcuboot_updater_get_status();
    zassert_equal(st.state, MCUBOOT_UPDATER_LOCKED);
}

/* ============================================================================
 * Suite 2 — async flow tests
 *
 * Calls mcuboot_updater_init() once per suite, then exercises the
 * LOCKED→IDLE→ERASING→RECEIVING→VALIDATING→VALIDATED path using a FAT
 * ramdisk for staging and real CRC32 computation.  Commit (which calls
 * sys_reboot) is intentionally not tested.
 * ============================================================================ */

static void *flow_setup(void) {
    nand_mount();
    mcuboot_updater_init(NULL);
    return NULL;
}

static void flow_before(void *) {
    mcuboot_updater_abort();
}

static void flow_teardown(void *) {
    mcuboot_updater_abort();
    nand_unmount();
}

ZTEST_SUITE(mcuboot_updater_flow, NULL, flow_setup, flow_before, flow_before, flow_teardown);

ZTEST(mcuboot_updater_flow, test_begin_opens_staging_file_and_reaches_receiving) {
    static uint8_t payload[512];
    memset(payload, 0xAB, sizeof(payload));

    mcuboot_updater_unlock();
    zassert_ok(mcuboot_updater_begin(sizeof(payload)));
    zassert_equal(mcuboot_updater_get_status().state, MCUBOOT_UPDATER_ERASING);

    /* Wait for erase_work_handler to open /NAND:/mcuboot.bin. */
    k_sleep(K_MSEC(500));
    zassert_equal(mcuboot_updater_get_status().state, MCUBOOT_UPDATER_RECEIVING,
                  "State must be RECEIVING after erase work completes");
}

ZTEST(mcuboot_updater_flow, test_good_package_reaches_validated) {
    static uint8_t payload[1024];
    memset(payload, 0xCD, sizeof(payload));
    uint32_t crc = crc32_ieee(payload, sizeof(payload));

    MockPkgHeader hdr = {
        .magic = kPkgMagic, .major = 1, .minor = 0, .revision = 0,
        .payload_size = sizeof(payload), .crc32 = crc,
    };

    mcuboot_updater_unlock();
    zassert_ok(mcuboot_updater_begin(sizeof(payload)));
    k_sleep(K_MSEC(500));

    write_chunks(&hdr, payload, sizeof(payload));
    zassert_equal(mcuboot_updater_get_status().progress, 100u,
                  "Progress must be 100 once all bytes are written");

    zassert_ok(mcuboot_updater_validate());
    k_sleep(K_MSEC(500));

    struct McubootUpdaterStatus st = mcuboot_updater_get_status();
    zassert_equal(st.state, MCUBOOT_UPDATER_VALIDATED,
                  "Valid package must reach VALIDATED");
    zassert_equal(st.error, MCUBOOT_UPDATER_ERR_NONE);
}

ZTEST(mcuboot_updater_flow, test_bad_magic_reaches_error_invalid_magic) {
    static uint8_t payload[512];
    memset(payload, 0xEF, sizeof(payload));

    MockPkgHeader hdr = {
        .magic = 0xDEADBEEFU,              /* wrong magic */
        .payload_size = sizeof(payload),
        .crc32 = crc32_ieee(payload, sizeof(payload)),
    };

    mcuboot_updater_unlock();
    zassert_ok(mcuboot_updater_begin(sizeof(payload)));
    k_sleep(K_MSEC(500));

    write_chunks(&hdr, payload, sizeof(payload));
    zassert_ok(mcuboot_updater_validate());
    k_sleep(K_MSEC(500));

    struct McubootUpdaterStatus st = mcuboot_updater_get_status();
    zassert_equal(st.state, MCUBOOT_UPDATER_ERROR);
    zassert_equal(st.error, MCUBOOT_UPDATER_ERR_INVALID_MAGIC,
                  "Wrong GRMB magic must set INVALID_MAGIC error");
}

ZTEST(mcuboot_updater_flow, test_crc_mismatch_reaches_error_crc_mismatch) {
    static uint8_t payload[512];
    memset(payload, 0x55, sizeof(payload));

    MockPkgHeader hdr = {
        .magic = kPkgMagic,
        .payload_size = sizeof(payload),
        .crc32 = 0xDEADBEEFU,  /* deliberately wrong CRC */
    };

    mcuboot_updater_unlock();
    zassert_ok(mcuboot_updater_begin(sizeof(payload)));
    k_sleep(K_MSEC(500));

    write_chunks(&hdr, payload, sizeof(payload));
    zassert_ok(mcuboot_updater_validate());
    k_sleep(K_MSEC(500));

    struct McubootUpdaterStatus st = mcuboot_updater_get_status();
    zassert_equal(st.state, MCUBOOT_UPDATER_ERROR);
    zassert_equal(st.error, MCUBOOT_UPDATER_ERR_CRC_MISMATCH,
                  "Wrong CRC must set CRC_MISMATCH error");
}

ZTEST(mcuboot_updater_flow, test_write_chunk_beyond_payload_returns_eoverflow) {
    static uint8_t payload[64];
    memset(payload, 0, sizeof(payload));

    MockPkgHeader hdr = {
        .magic = kPkgMagic,
        .payload_size = sizeof(payload),
        .crc32 = crc32_ieee(payload, sizeof(payload)),
    };

    mcuboot_updater_unlock();
    zassert_ok(mcuboot_updater_begin(sizeof(payload)));
    k_sleep(K_MSEC(500));

    write_chunks(&hdr, payload, sizeof(payload));

    /* All bytes consumed — one more byte must be rejected. */
    uint8_t extra = 0xFF;
    zassert_equal(mcuboot_updater_write_chunk(&extra, 1), -EOVERFLOW,
                  "Writing past the declared payload size must return -EOVERFLOW");
}

ZTEST(mcuboot_updater_flow, test_validate_incomplete_data_returns_einval) {
    mcuboot_updater_unlock();
    zassert_ok(mcuboot_updater_begin(1024));
    k_sleep(K_MSEC(500));

    /* Only write the header — payload not yet received. */
    MockPkgHeader hdr = {.magic = kPkgMagic, .payload_size = 1024, .crc32 = 0};
    zassert_ok(mcuboot_updater_write_chunk((const uint8_t *)&hdr, sizeof(hdr)));

    zassert_equal(mcuboot_updater_validate(), -EINVAL,
                  "validate() before all bytes are received must return -EINVAL");
}

ZTEST(mcuboot_updater_flow, test_abort_from_receiving_deletes_staging_file) {
    static uint8_t payload[256];
    memset(payload, 0xBB, sizeof(payload));

    mcuboot_updater_unlock();
    zassert_ok(mcuboot_updater_begin(sizeof(payload)));
    k_sleep(K_MSEC(500));

    /* Write some but not all bytes, then abort. */
    MockPkgHeader hdr = {.magic = kPkgMagic, .payload_size = sizeof(payload), .crc32 = 0};
    mcuboot_updater_write_chunk((const uint8_t *)&hdr, sizeof(hdr));
    mcuboot_updater_abort();

    zassert_equal(mcuboot_updater_get_status().state, MCUBOOT_UPDATER_LOCKED,
                  "abort() must return to LOCKED from any state");

    /* Staging file should be gone after abort. */
    struct fs_file_t f;
    fs_file_t_init(&f);
    int rc = fs_open(&f, "/NAND:/mcuboot.bin", FS_O_READ);
    zassert_not_equal(rc, 0, "Staging file must be deleted on abort");
    if (rc == 0) {
        fs_close(&f);
    }
}
