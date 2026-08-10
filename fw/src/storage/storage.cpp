#include <ff.h>
#include <storage/storage.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
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

/* Erase length for the corruption. The MX25R6435F erases in 4 KB sectors and
 * flash_area_erase() requires a whole number of them; one sector is enough, since it
 * takes out the boot sector (the 0xAA55 signature at offset 510 that FatFs checks) and
 * the start of the first FAT. */
static constexpr size_t kCorruptEraseLen = 4096;

/*
 * `fatfs corrupt confirm` — makes the volume unmountable so the NEXT boot exercises the
 * auto-format path: fs_mount() fails, CONFIG_FS_FATFS_MOUNT_MKFS reformats, and all of
 * that happens on the MAIN thread inside SYS_INIT(mount_fat, APPLICATION).
 *
 * That path is otherwise untestable, and it is the one that matters for
 * CONFIG_MAIN_STACK_SIZE (issue #105): an undersized main stack boot-loops a device whose
 * filesystem is already broken, which is recoverable only with a J-Link. It is also the
 * path a field device takes after a corrupting power loss, so "it works" should not be an
 * assumption.
 *
 * TWO THINGS HERE ARE NOT OPTIONAL, both learned by watching the naive versions fail:
 *
 * 1. UNMOUNT FIRST. A mounted FATFS holds cached FAT and directory structures and writes
 *    them back on its own schedule, so corrupting underneath a live mount is silently
 *    undone — the volume comes back intact on the next boot and the test looks like it
 *    passed when it never ran.
 *
 * 2. DO NOT REMOUNT AFTERWARDS. Remounting here would immediately trigger the same
 *    auto-format in THIS boot's context (on the shell thread), repairing the volume and
 *    destroying the very condition the next boot is supposed to find. The volume is
 *    deliberately left unmounted and every /NAND: access fails until the reboot.
 *
 * Writing to the partition from the USB host instead does not work either, for reason 1:
 * the firmware's cached FAT wins. Verified on proto0 — a host-side zeroing of the boot
 * sector read back as zeros from the host and the volume still mounted intact.
 */
static int cmd_storage_corrupt(const struct shell *sh, size_t argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "confirm") != 0) {
        shell_error(sh, "Refusing without explicit confirmation.");
        shell_print(sh, "Usage: fatfs corrupt confirm");
        shell_print(sh, "Erases the FAT boot sector so the NEXT boot auto-formats on the");
        shell_print(sh, "main thread. ALL FILES ARE LOST and %s is unusable until reboot.",
                    fat_mnt.mnt_point);
        return -EINVAL;
    }

    shell_warn(sh, "Corrupting %s — all files will be lost.", fat_mnt.mnt_point);

    /* See note 1 above: this must happen before the erase. */
    int rc = fs_unmount(&fat_mnt);
    if (rc < 0) {
        shell_error(sh, "Unmount failed: %d — aborting (a live mount would undo the erase)",
                    rc);
        return rc;
    }

    const struct flash_area *fa = NULL;
    rc = flash_area_open(FIXED_PARTITION_ID(fat_storage), &fa);
    if (rc < 0) {
        shell_error(sh, "flash_area_open failed: %d", rc);
        (void)fs_mount(&fat_mnt);  /* nothing was changed; put it back */
        return rc;
    }

    rc = flash_area_erase(fa, 0, kCorruptEraseLen);
    flash_area_close(fa);
    if (rc < 0) {
        shell_error(sh, "flash_area_erase failed: %d", rc);
        /* The erase may have partially completed, so the volume's state is unknown —
         * remounting could either work or trigger an auto-format. Say so rather than
         * silently doing one or the other. */
        shell_warn(sh, "Volume left unmounted; state unknown. Reboot, then `fatfs reformat`");
        return rc;
    }

    /* See note 2: deliberately NOT remounted. */
    shell_print(sh, "Boot sector erased; %s left UNMOUNTED.", fat_mnt.mnt_point);
    shell_print(sh, "Reboot now (`kernel reboot cold`). Expect on the next boot:");
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
