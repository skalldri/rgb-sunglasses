/*
 * Stub implementations of functions not available on native_sim.
 *
 * bootmode_*: require retained RAM hardware; mcuboot_updater_init() handles
 *   -ENOSYS from bootmode_check() gracefully (sets flash_unlocked=0).
 *
 * sys_reboot: commit_work_handler calls this after flashing. The stub just
 *   returns so tests can verify the DONE state after a full commit flow.
 */
#include <errno.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>

int bootmode_check(uint8_t boot_mode) { (void)boot_mode; return -ENOSYS; }
int bootmode_set(uint8_t boot_mode)   { (void)boot_mode; return 0; }
int bootmode_clear(void)              { return 0; }

void sys_reboot(int type)
{
    (void)type;
    /* In native_sim, simulated time only advances when threads yield.  A
     * for(;;) spin would starve the test thread's k_sleep timer and deadlock.
     * k_thread_abort terminates this work-queue thread immediately — DONE state
     * is already set before this call — allowing the test thread to wake up and
     * observe it.
     *
     * k_thread_abort is not declared FUNC_NORETURN, so CODE_UNREACHABLE tells
     * the compiler that control never reaches the end of this noreturn function. */
    k_thread_abort(k_current_get());
    CODE_UNREACHABLE;
}
