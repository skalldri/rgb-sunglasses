#include <ff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

LOG_MODULE_REGISTER(storage);

static FATFS fat_fs;

/* Mount point "/NAND" maps to the "NAND" disk registered by the zephyr,flash-disk
 * driver (fat_partition on the MX25R64, offset 0x12E000, 4 MB).
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

#if defined(CONFIG_SHELL) && defined(CONFIG_FILE_SYSTEM_MKFS)

// ELM FAT logical drive name: translate_path() strips the leading '/' from the
// mount point, so "/NAND:" -> "NAND:" is what f_mkfs / fs_mkfs expect.
static constexpr const char *kFatDiskId = "NAND:";

static int cmd_storage_reformat(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_warn(sh, "Reformatting %s — all files will be erased.", fat_mnt.mnt_point);

    int rc = fs_unmount(&fat_mnt);
    if (rc < 0) {
        shell_error(sh, "Unmount failed: %d", rc);
        return rc;
    }

    rc = fs_mkfs(FS_FATFS, (uintptr_t)kFatDiskId, NULL, 0);
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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_storage,
    SHELL_CMD(reformat, NULL, "Reformat the NAND FAT filesystem (DESTRUCTIVE — erases all files)",
              cmd_storage_reformat),
    SHELL_SUBCMD_SET_END);
/* "storage" is a reserved macro in nrf/include/flash_map_pm.h — use "fatfs" instead. */
SHELL_CMD_REGISTER(fatfs, &sub_storage, "FAT filesystem management", NULL);

#endif /* CONFIG_SHELL && CONFIG_FILE_SYSTEM_MKFS */

#if defined(CONFIG_SHELL)

static int cmd_appcfg_erase(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    const struct flash_area *fa;
    int rc = flash_area_open(FIXED_PARTITION_ID(settings_storage), &fa);
    if (rc) {
        shell_error(sh, "Failed to open settings partition: %d", rc);
        return rc;
    }

    shell_warn(sh, "Erasing settings partition (%u bytes) — all config will reset to defaults on next boot.",
               fa->fa_size);

    rc = flash_area_erase(fa, 0, fa->fa_size);
    flash_area_close(fa);

    if (rc) {
        shell_error(sh, "Erase failed: %d", rc);
        return rc;
    }

    shell_print(sh, "Done. Reboot to apply.");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_appcfg,
    SHELL_CMD(erase, NULL,
              "Erase the NVS settings partition (DESTRUCTIVE — resets all config on next boot)",
              cmd_appcfg_erase),
    SHELL_SUBCMD_SET_END);
/* "appcfg" matches the key prefix used in persistent_value_store.cpp. */
SHELL_CMD_REGISTER(appcfg, &sub_appcfg, "Application config (NVS settings) management", NULL);

#endif /* CONFIG_SHELL */
