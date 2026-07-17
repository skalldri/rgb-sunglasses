#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_FILE_SYSTEM_MKFS)
/* Re-create the FAT filesystem on the NAND flash disk. The volume must NOT be
 * mounted when this is called — fs_mkfs() writes filesystem metadata straight
 * through the disk layer and does not consult the mount table. Boot-time
 * callers (factory reset) run before mount_fat's SYS_INIT; the fatfs shell
 * command brackets it with fs_unmount/fs_mount itself.
 * Returns 0 on success, negative errno on failure. */
int storage_fat_mkfs_unmounted(void);

/* Factory-reset variant: unmounts /NAND: first if it is mounted (tolerating
 * "not mounted", so it also works at boot before mount_fat runs), then
 * re-creates the filesystem. The volume is left UNMOUNTED — callers are
 * expected to reboot afterwards. Returns 0 on success, negative errno. */
int storage_fat_wipe_for_reset(void);
#endif

#ifdef __cplusplus
}
#endif
