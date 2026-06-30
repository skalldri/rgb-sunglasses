/*
 * Stub implementations of functions not available on native_sim.
 *
 * bootmode_*: require retained RAM hardware; mcuboot_updater_init() handles
 *   -ENOSYS from bootmode_check() gracefully (sets flash_unlocked=0).
 *
 * sys_reboot: commit_work_handler calls this after flashing; the test suite
 *   intentionally never calls commit(), so this stub is a safety net only.
 */
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>

int bootmode_check(uint8_t boot_mode) { (void)boot_mode; return -ENOSYS; }
int bootmode_set(uint8_t boot_mode)   { (void)boot_mode; return 0; }
int bootmode_clear(void)              { return 0; }

void sys_reboot(int type)
{
    (void)type;
    /* Should never be called — commit() is not tested. Spin as a safety net. */
    for (;;) { }
}
