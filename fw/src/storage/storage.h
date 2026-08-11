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

#if defined(CONFIG_APP_CRASH_TEST_COMMANDS)
/* TEST AID: make the FAT volume unmountable, so the NEXT boot exercises the
 * CONFIG_FS_FATFS_MOUNT_MKFS auto-format path — which runs on the MAIN thread inside
 * SYS_INIT(mount_fat, APPLICATION) and is otherwise unreachable. See the shell command
 * in storage.cpp for the full rationale and the caller's responsibilities.
 *
 * The volume must already be UNMOUNTED (the caller owns that; a live mount writes its
 * cached FAT back over the erase). Flushes the flashdisk driver's page cache, erases the
 * first flash sector, then READS BACK offset 510 to confirm the 0xAA55 boot signature is
 * really gone — a partial erase that leaves the signature intact would otherwise report
 * success while the next boot mounts normally and never auto-formats.
 *
 * Returns 0 when the volume is confirmed unmountable, -EIO if the signature survived,
 * or a negative errno from the flash layer. */
int storage_fat_corrupt_boot_sector(void);
#endif

#ifdef __cplusplus
}
#endif
