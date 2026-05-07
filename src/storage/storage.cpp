#include <zephyr/fs/fs.h>
#include <ff.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_REGISTER(storage);

static FATFS fat_fs;

/* Mount point "/NAND" maps to the "NAND" disk registered by the zephyr,flash-disk
 * driver (fat_partition on the MX25R64, offset 0x12E000, 4 MB).
 * CONFIG_FS_FATFS_MOUNT_MKFS=y auto-formats the partition on first boot. */
static struct fs_mount_t fat_mnt = {
    .type      = FS_FATFS,
    .mnt_point = "/NAND:",
    .fs_data   = &fat_fs,
    .storage_dev = (void*)FIXED_PARTITION_ID(fat_storage),
};

static int mount_fat(void)
{
    int rc = fs_mount(&fat_mnt);
    if (rc < 0) {
        LOG_ERR("FAT mount failed: %d", rc);
    } else {
        LOG_INF("FAT mounted at %s", fat_mnt.mnt_point);
    }
    return rc;

    return 0;
}

SYS_INIT(mount_fat, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
