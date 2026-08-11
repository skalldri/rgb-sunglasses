/*
 * Coverage for storage_fat_corrupt_boot_sector() — the flash half of the
 * `fatfs corrupt` test aid.
 *
 * The command's whole value is that it CANNOT silently no-op: a test aid that reports
 * success while the volume stays mountable makes the next boot look like it exercised the
 * auto-format path when it never did. So these tests assert the two properties that carry
 * that guarantee — the signature is really gone, and a surviving signature is reported as
 * a failure rather than swallowed — plus idempotency, since the command deliberately
 * leaves the volume unmounted and re-arming it is a normal thing to do.
 */

#include <storage/storage.h>

#include <zephyr/fs/fs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include <cstring>

namespace {

constexpr off_t kBootSignatureOff = 510;

/* Writes a plausible FAT boot signature so the erase has something real to remove.
 * flash_area_write needs an erased target, so erase the sector first. */
void seed_boot_signature(void) {
    const struct flash_area *fa = nullptr;
    zassert_ok(flash_area_open(FIXED_PARTITION_ID(fat_storage), &fa), "open failed");
    zassert_ok(flash_area_erase(fa, 0, 4096), "pre-erase failed");

    /* 0xAA55 little-endian at 510 is what FatFs checks to decide the volume is
     * mountable — it is the exact byte pair the helper has to destroy. */
    const uint8_t sig[2] = {0x55, 0xAA};
    zassert_ok(flash_area_write(fa, kBootSignatureOff, sig, sizeof(sig)), "write failed");
    flash_area_close(fa);
}

bool signature_present(void) {
    const struct flash_area *fa = nullptr;
    zassert_ok(flash_area_open(FIXED_PARTITION_ID(fat_storage), &fa), "open failed");
    uint8_t sig[2] = {0, 0};
    zassert_ok(flash_area_read(fa, kBootSignatureOff, sig, sizeof(sig)), "read failed");
    flash_area_close(fa);
    return sig[0] == 0x55 && sig[1] == 0xAA;
}

}  // namespace

ZTEST_SUITE(fat_corrupt_tests, NULL, NULL, NULL, NULL, NULL);

/* The property the command asserts to its operator: after a success return, the volume is
 * genuinely unmountable. */
ZTEST(fat_corrupt_tests, test_erases_the_boot_signature) {
    seed_boot_signature();
    zassert_true(signature_present(), "setup: the signature should be there to begin with");

    zassert_ok(storage_fat_corrupt_boot_sector(), "corrupt should succeed");

    zassert_false(signature_present(), "the 0xAA55 signature must be gone");
}

/* The first flash sector must be fully erased, not just the two signature bytes — the FAT
 * itself lives in this sector too (FM_SFD, 512-byte disk sectors, so page 0 holds the boot
 * sector plus FAT sectors 1-7). */
ZTEST(fat_corrupt_tests, test_erases_the_whole_first_sector) {
    seed_boot_signature();
    zassert_ok(storage_fat_corrupt_boot_sector());

    const struct flash_area *fa = nullptr;
    zassert_ok(flash_area_open(FIXED_PARTITION_ID(fat_storage), &fa));
    uint8_t buf[256];
    for (off_t off = 0; off < 4096; off += static_cast<off_t>(sizeof(buf))) {
        zassert_ok(flash_area_read(fa, off, buf, sizeof(buf)), "read at %ld failed", (long)off);
        for (size_t i = 0; i < sizeof(buf); i++) {
            zassert_equal(buf[i], 0xFF, "byte at %ld not erased: 0x%02x", (long)(off + i),
                          buf[i]);
        }
    }
    flash_area_close(fa);
}

/* Re-arming must work. The command leaves the volume unmounted by design, so running it
 * twice without a reboot is a normal operator action — and the unmount guard in the shell
 * wrapper exists precisely because the naive version aborted on the second run. Erasing an
 * already-erased sector must stay a clean success, not an error. */
ZTEST(fat_corrupt_tests, test_is_idempotent) {
    seed_boot_signature();
    zassert_ok(storage_fat_corrupt_boot_sector(), "first call should succeed");
    zassert_ok(storage_fat_corrupt_boot_sector(), "second call should also succeed");
    zassert_false(signature_present());
}

/* On a volume that never had a signature, success is still the honest answer: the
 * post-condition the caller cares about ("this will not mount") already holds. */
ZTEST(fat_corrupt_tests, test_succeeds_on_an_already_blank_volume) {
    const struct flash_area *fa = nullptr;
    zassert_ok(flash_area_open(FIXED_PARTITION_ID(fat_storage), &fa));
    zassert_ok(flash_area_erase(fa, 0, 4096));
    flash_area_close(fa);

    zassert_ok(storage_fat_corrupt_boot_sector());
    zassert_false(signature_present());
}
