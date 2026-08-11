#include <ff.h>
#include <mcuboot_updater.h>
#include <storage/storage.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/storage/disk_access.h>
#include <cstring>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

LOG_MODULE_REGISTER(storage);

static FATFS fat_fs;

/* Mount point "/NAND" maps to the "NAND" disk registered by the zephyr,flash-disk
 * driver (fat_partition on the MX25R64, offset 0x124000, ~6.9 MB).
 * CONFIG_FS_FATFS_MOUNT_MKFS=y auto-formats the partition on first boot. */
static struct fs_mount_t fat_mnt = {
    .type = FS_FATFS,
    .mnt_point = "/NAND:",
    .fs_data = &fat_fs,
    .storage_dev = (void*)FIXED_PARTITION_ID(fat_storage),
};

static int mount_fat(void) {
    int rc = fs_mount(&fat_mnt);
    if (rc < 0) {
        LOG_ERR("FAT mount failed: %d", rc);
    } else {
        LOG_INF("FAT mounted at %s", fat_mnt.mnt_point);
    }
    return rc;
}

SYS_INIT(mount_fat, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if defined(CONFIG_FILE_SYSTEM_MKFS)

// ELM FAT logical drive name: translate_path() strips the leading '/' from the
// mount point, so "/NAND:" -> "NAND:" is what f_mkfs / fs_mkfs expect.
static constexpr const char *kFatDiskId = "NAND:";

int storage_fat_mkfs_unmounted(void) {
    return fs_mkfs(FS_FATFS, (uintptr_t)kFatDiskId, NULL, 0);
}

int storage_fat_wipe_for_reset(void) {
    // Skip the unmount when the volume isn't mounted (factory reset at boot
    // runs before mount_fat's SYS_INIT) instead of letting fs_unmount() fail
    // with -EINVAL — the SDK logs its own LOG_ERR for that expected case.
    // sys_dnode_is_linked on the mount node is fs_unmount()'s own
    // mounted-ness test (zephyr/subsys/fs/fs.c).
    if (sys_dnode_is_linked(&fat_mnt.node)) {
        int rc = fs_unmount(&fat_mnt);
        if (rc < 0) {
            return rc;
        }
    }
    return storage_fat_mkfs_unmounted();
}

#endif /* CONFIG_FILE_SYSTEM_MKFS */

#if defined(CONFIG_APP_CRASH_TEST_COMMANDS)
/* Deliberately OUTSIDE the CONFIG_SHELL guard below: this is the testable half of
 * the `fatfs corrupt` aid, and the native_sim suite that covers it
 * (fw/tests/storage/fat_corrupt) builds no shell. */
/* Erase length. The MX25R6435F erases in 4 KB sectors and flash_area_erase() requires a
 * whole number of them; one sector covers the boot sector (the 0xAA55 signature at offset
 * 510 that FatFs checks) and, with FM_SFD and 512-byte disk sectors, FAT sectors 1-7. */
static constexpr size_t kCorruptEraseLen = 4096;
static constexpr off_t kBootSignatureOff = 510;

int storage_fat_corrupt_boot_sector(void) {
    /* Flush the flashdisk driver's 4 KB page cache BEFORE touching flash.
     *
     * fs_unmount() is not enough. Its DISK_IOCTL_CTRL_DEINIT reaches
     * disk_flash_access_ioctl() in zephyr/drivers/disk/flashdisk.c, which commits the
     * cached page but leaves ctx->cache_valid set — and page 0 holds the boot sector plus
     * FAT sectors 1-7, so page 0 is very often the resident page after any file write.
     * Without this flush a dirty cached page can be committed AFTER our erase, silently
     * restoring a mountable volume and making the test pass without ever running.
     *
     * RESIDUAL, and the reason the shell command tells the operator to reboot immediately:
     * this flushes, it does not invalidate. A host write into page 0 over USB MSC before
     * the reboot would still go through flashdisk_cache_load(), hit cached_addr ==
     * fl_addr, and commit 4096 pre-erase bytes back to flash. The read-back below cannot
     * see that coming — it reads flash directly, so it reports the truth at this instant. */
    (void)disk_access_ioctl("NAND", DISK_IOCTL_CTRL_DEINIT, NULL);

    const struct flash_area *fa = NULL;
    int rc = flash_area_open(FIXED_PARTITION_ID(fat_storage), &fa);
    if (rc < 0) {
        return rc;
    }

    rc = flash_area_erase(fa, 0, kCorruptEraseLen);
    if (rc < 0) {
        flash_area_close(fa);
        return rc;
    }

    /* Confirm the volume is genuinely unmountable rather than trusting the erase's return
     * code. A partial erase (QSPI timeout, write-protect) can clear some pages and leave
     * the one holding offset 510 intact — f_mount then returns FR_OK, no auto-format runs,
     * and the caller is left with a volume that looks healthy while its FAT is partly
     * gone. That is strictly worse than a loud failure, because the next boot writes into
     * a corrupt allocation table. */
    uint8_t sig[2] = {0, 0};
    rc = flash_area_read(fa, kBootSignatureOff, sig, sizeof(sig));
    flash_area_close(fa);
    if (rc < 0) {
        return rc;
    }
    if (sig[0] == 0x55 && sig[1] == 0xAA) {
        return -EIO;
    }
    return 0;
}

#endif /* CONFIG_APP_CRASH_TEST_COMMANDS */

#if defined(CONFIG_SHELL) && defined(CONFIG_FILE_SYSTEM_MKFS)

static int cmd_storage_reformat(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_warn(sh, "Reformatting %s — all files will be erased.", fat_mnt.mnt_point);

    int rc = fs_unmount(&fat_mnt);
    if (rc < 0) {
        shell_error(sh, "Unmount failed: %d", rc);
        return rc;
    }

    rc = storage_fat_mkfs_unmounted();
    if (rc < 0) {
        shell_error(sh, "Format failed: %d", rc);
        int remount_rc = fs_mount(&fat_mnt);
        if (remount_rc < 0) {
            shell_error(sh, "Remount after failed format also failed: %d", remount_rc);
        }
        return rc;
    }

    rc = fs_mount(&fat_mnt);
    if (rc < 0) {
        shell_error(sh, "Remount failed: %d", rc);
        return rc;
    }

    shell_print(sh, "Done. Reset the board for glim_registry to rescan.");
    return 0;
}

#if defined(CONFIG_APP_CRASH_TEST_COMMANDS)
/*
 * `fatfs corrupt confirm` — makes the volume unmountable so the NEXT boot exercises the
 * auto-format path: fs_mount() fails, CONFIG_FS_FATFS_MOUNT_MKFS reformats, and all of
 * that happens on the MAIN thread inside SYS_INIT(mount_fat, APPLICATION).
 *
 * That path is otherwise untestable, and it is the one that matters for
 * CONFIG_MAIN_STACK_SIZE (issue #105): an undersized main stack boot-loops a device whose
 * filesystem is already broken, which is recoverable only with a J-Link. It is also the
 * path a field device takes after a corrupting power loss.
 *
 * THE HARD PART IS NOT THE ERASE, IT IS MAKING SURE NOTHING PUTS IT BACK. Four things can
 * silently undo it, and a test aid that silently passes is worse than none:
 *
 *  1. A live FATFS mount writes its cached structures back. Hence the unmount below.
 *  2. The flashdisk driver's own page cache — handled in the helper above.
 *  3. Remounting here would trigger the auto-format immediately, in THIS boot on the shell
 *     thread, repairing the volume. So the volume is deliberately left unmounted and every
 *     /NAND: access fails until the reboot.
 *  4. The USB host. USBD_DEFINE_MSC_LUN binds to the DISK, not the mount, so the LUN stays
 *     live and writable through all of this; a host with /NAND: mounted flushes its own
 *     cached boot sector on the next write, sync or umount. Nothing firmware-side can stop
 *     that, so the command says so and tells the operator to unmount host-side first.
 *
 * Writing to the partition from the host instead does not work either, for reason 1:
 * verified on proto0 that a host-side zeroing read back as zeros FROM THE HOST while the
 * volume still mounted intact on the next boot.
 */
static int cmd_storage_corrupt(const struct shell *sh, size_t argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "confirm") != 0) {
        shell_error(sh, "Refusing without explicit confirmation.");
        shell_print(sh, "Usage: fatfs corrupt confirm");
        shell_print(sh, "Erases the FAT boot sector so the NEXT boot auto-formats on the");
        shell_print(sh, "main thread. ALL FILES ARE LOST and %s is unusable until reboot.",
                    fat_mnt.mnt_point);
        shell_print(sh, "Unmount the USB disk on the host FIRST — a host-side cache flush");
        shell_print(sh, "will rewrite a valid boot sector and the test will silently pass.");
        return -EINVAL;
    }

    /* Refuse while the bootloader updater owns /NAND:. commit_work_handler() interleaves
     * fs_read() of /NAND:/mcuboot.bin with erase/write of the internal MCUboot partition,
     * page by page, driven by the app over BLE — invisible to whoever is typing here.
     * fs_unmount() succeeds with that file open and invalidates the FIL, so the next
     * fs_read fails AFTER pages 1..p-1 of the bootloader have been rewritten, leaving
     * MCUboot half-updated and the board dependent on J-Link or serial recovery. */
    const struct McubootUpdaterStatus updater = mcuboot_updater_get_status();
    if (updater.state != MCUBOOT_UPDATER_LOCKED && updater.state != MCUBOOT_UPDATER_IDLE) {
        shell_error(sh, "Refusing: MCUboot updater is busy (state %d).", (int)updater.state);
        shell_print(sh, "A bootloader commit reads /NAND:/mcuboot.bin page by page; pulling");
        shell_print(sh, "the filesystem out from under it can leave MCUboot half-written.");
        shell_print(sh, "Wait for it to finish, or `mcuboot_update abort`, then retry.");
        return -EBUSY;
    }

    shell_warn(sh, "Corrupting %s — all files will be lost.", fat_mnt.mnt_point);

    /* Guarded because the volume may already be unmounted — including by a previous run of
     * this very command, which leaves it that way by design. fs_unmount() returns -EINVAL
     * (plus its own SDK LOG_ERR) when the mount node is not linked, and aborting on that
     * would block re-arming the test without a reboot, citing a live mount that does not
     * exist. Same guard storage_fat_wipe_for_reset() uses above, for the same reason. */
    if (sys_dnode_is_linked(&fat_mnt.node)) {
        int rc = fs_unmount(&fat_mnt);
        if (rc < 0) {
            shell_error(sh, "Unmount failed: %d — aborting (a live mount would undo the erase)",
                        rc);
            return rc;
        }
    }

    int rc = storage_fat_corrupt_boot_sector();
    if (rc == -EIO) {
        shell_error(sh, "Erase reported success but the 0xAA55 signature survived.");
        shell_warn(sh, "The volume is still mountable, so the next boot will NOT auto-format.");
        shell_warn(sh, "Its FAT may be partly erased — run `fatfs reformat` before using it.");
        return rc;
    }
    if (rc < 0) {
        shell_error(sh, "Corrupt failed: %d", rc);
        /* State is unknown: the erase may have partially completed. Report the remount
         * result rather than discarding it — a silent failure here leaves /NAND: gone for
         * the rest of the boot (GLIM playback, extension loads and the coredump drain all
         * fail) with nothing saying so. */
        int remount_rc = fs_mount(&fat_mnt);
        if (remount_rc < 0) {
            shell_error(sh, "Remount after failed corrupt also failed: %d — %s is UNMOUNTED",
                        remount_rc, fat_mnt.mnt_point);
        } else {
            shell_warn(sh, "Remounted, but the volume state is unknown; `fatfs reformat` it.");
        }
        return rc;
    }

    /* See note 3: deliberately NOT remounted. */
    shell_print(sh, "Boot sector erased and verified gone; %s left UNMOUNTED.",
                fat_mnt.mnt_point);
    shell_print(sh, "Reboot NOW — do not write to the USB disk from the host first.");
    shell_print(sh, "Expect on the next boot:");
    shell_print(sh, "  - a mount failure, then an automatic format on the main thread");
    shell_print(sh, "  - an empty volume (the registries recreate their own directories)");
    shell_print(sh, "  - `kernel thread stacks` showing main's high-water for that path");
    return 0;
}

#endif /* CONFIG_APP_CRASH_TEST_COMMANDS */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_storage,
    SHELL_CMD(reformat, NULL, "Reformat the NAND FAT filesystem (DESTRUCTIVE — erases all files)",
              cmd_storage_reformat),
#if defined(CONFIG_APP_CRASH_TEST_COMMANDS)
    SHELL_CMD_ARG(corrupt, NULL,
                  "DESTRUCTIVE test aid: erase the boot sector so the next boot auto-formats",
                  cmd_storage_corrupt, 1, 1),
#endif
    SHELL_SUBCMD_SET_END);
/* "storage" is a reserved macro in nrf/include/flash_map_pm.h — use "fatfs" instead. */
SHELL_CMD_REGISTER(fatfs, &sub_storage, "FAT filesystem management", NULL);

#endif /* CONFIG_SHELL && CONFIG_FILE_SYSTEM_MKFS */
