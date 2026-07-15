#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <cstdint>

/* Deliberate-crash shell commands (issue #80) for validating the coredump
 * pipeline end-to-end: fault → coredump captured to the internal flash
 * partition (z_fatal_error runs the coredump backend BEFORE the fatal error
 * handler) → reboot (k_sys_fatal_error_handler in extension_host.cpp, unless
 * a debugger is attached) → coredump_manager copies the dump to
 * /NAND:/coredump on the next boot. */

namespace {

int cmd_crash_panic(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_warn(shell, "Triggering k_panic()...");
    k_panic();
    return 0;  // not reached
}

int cmd_crash_mpu(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_warn(shell, "Writing to read-only flash to trigger an MPU fault...");
    /* With CONFIG_USERSPACE the ARM MPU maps the flash region read-only even
     * for privileged threads, so this store raises a precise MemManage
     * (DACCVIOL) fault with a valid exception frame. The pointer is built
     * from a volatile so the compiler can't fold it into UB or optimize the
     * store away. Address 4 (vector table, reset vector) rather than literal
     * NULL avoids null-pointer special-casing. */
    volatile uintptr_t addr = 4;
    *reinterpret_cast<volatile uint32_t*>(addr) = 0xDEADBEEF;
    return 0;  // not reached
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_crash,
    SHELL_CMD(panic, NULL, "k_panic() — kernel panic with coredump capture", cmd_crash_panic),
    SHELL_CMD(mpu, NULL, "Write to RO flash — MPU fault with coredump capture", cmd_crash_mpu),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(crash, &sub_crash, "Deliberately crash the firmware (coredump test)", NULL);

}  // namespace
