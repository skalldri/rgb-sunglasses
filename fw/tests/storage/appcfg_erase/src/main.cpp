#include <storage/appcfg_erase.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>
#include <cstring>

ZTEST_SUITE(appcfg_erase_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(appcfg_erase_tests, test_erase_clears_partition_to_0xff) {
    const struct flash_area *fa;
    zassert_ok(flash_area_open(FIXED_PARTITION_ID(settings_storage), &fa),
               "Failed to open settings_storage");

    // Pre-condition: erase then write a known non-0xFF pattern.
    zassert_ok(flash_area_erase(fa, 0, fa->fa_size), "pre-erase failed");

    uint8_t pattern[64];
    memset(pattern, 0xAA, sizeof(pattern));
    zassert_ok(flash_area_write(fa, 0, pattern, sizeof(pattern)), "write failed");
    flash_area_close(fa);

    // Function under test
    zassert_ok(storage_erase_settings_partition(), "erase should succeed");

    // Post-condition: partition should read as all 0xFF.
    zassert_ok(flash_area_open(FIXED_PARTITION_ID(settings_storage), &fa),
               "re-open after erase failed");
    uint8_t buf[64];
    zassert_ok(flash_area_read(fa, 0, buf, sizeof(buf)), "read after erase failed");
    flash_area_close(fa);

    for (size_t i = 0; i < sizeof(buf); i++) {
        zassert_equal(buf[i], 0xFF,
                      "byte[%zu] = 0x%02X, expected 0xFF after erase", i, buf[i]);
    }
}

ZTEST(appcfg_erase_tests, test_erase_is_idempotent) {
    // Erasing an already-blank partition must succeed with no errors.
    zassert_ok(storage_erase_settings_partition(), "first erase should succeed");
    zassert_ok(storage_erase_settings_partition(), "second erase should also succeed");
}
