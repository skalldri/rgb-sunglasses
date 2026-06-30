/*
 * fprotect_hook.c — conditional flash-protection boot hook for MCUboot.
 *
 * CONFIG_FPROTECT=n is set in MCUboot's sysbuild conf so that MCUboot never
 * calls fprotect_area() itself.  This hook re-implements that protection, but
 * first inspects the retention boot-mode register set by the application.
 *
 * Protocol (one byte in the zephyr,boot-mode retention device, gpregret2):
 *
 *   BOOT_MODE_TYPE_NORMAL  (0x00) — normal boot: apply fprotect, clear mode.
 *   BOOT_MODE_UPDATER_REQ  (0xB1) — app requests updater boot: skip fprotect,
 *                                   write BOOT_MODE_UPDATER_ACTIVE so the app
 *                                   can confirm the region is unlocked.
 *   BOOT_MODE_UPDATER_ACTIVE (0xB2) — stale from a previous updater boot:
 *                                   treat as normal (apply fprotect, clear).
 *   Any other value               — treat as normal.
 *
 * After a successful commit the device reboots normally (mode == 0xB2 from the
 * previous updater-mode boot). The hook sees 0xB2 ≠ 0xB1, applies fprotect,
 * and clears the mode — restoring full protection for the new MCUboot image.
 */

#include <zephyr/kernel.h>
#include <zephyr/retention/bootmode.h>
#include <fprotect.h>
#include <pm_config.h>
#include "bootutil/boot_hooks.h"
#include "bootutil/fault_injection_hardening.h"

/* Must match MCUBOOT_UPDATER_BOOT_MODE_* in fw/src/mcuboot_updater.h */
#define BOOT_MODE_UPDATER_REQ    0xB1U
#define BOOT_MODE_UPDATER_ACTIVE 0xB2U

fih_ret boot_go_hook(struct boot_rsp *rsp)
{
    int rc;

    rc = bootmode_check(BOOT_MODE_UPDATER_REQ);
    if (rc == 1) {
        /* Updater-mode boot: leave MCUboot flash unprotected.
         * Write ACTIVE so the app can verify the region is writable. */
        bootmode_set(BOOT_MODE_UPDATER_ACTIVE);
    } else {
        /* Normal boot (or post-commit reboot with ACTIVE still set):
         * protect MCUboot's own flash region exactly as CONFIG_FPROTECT=y would. */
        bootmode_clear();

#if USE_PARTITION_MANAGER
        rc = fprotect_area(PM_MCUBOOT_ADDRESS,
                           PM_MCUBOOT_PRIMARY_ADDRESS - PM_MCUBOOT_ADDRESS);
        if (rc != 0) {
            /* Mirror MCUboot's own fprotect failure handling. */
            while (1) {
            }
        }
#endif
    }

    return FIH_BOOT_HOOK_REGULAR;
}
