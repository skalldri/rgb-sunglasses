/*
 * Runtime-neutral fault containment for unprivileged extension sandboxes.
 *
 * Zephyr aborts only the faulting nonessential thread when this handler
 * returns. It does so exclusively for the exact active LLEXT or Wasm sandbox
 * thread and never demotes a kernel panic. Every other fault keeps the stock
 * halt or coredump-reboot policy used for firmware defects.
 */

#if defined(CONFIG_APP_EXTENSION_HOST)
#include <extensions/extension_host.h>
#endif
#if defined(CONFIG_APP_WASM3_MVP)
#include <extensions/wasm_mvp_runtime.h>
#endif

#include <cmsis_core.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(sandbox_fatal, LOG_LEVEL_INF);

namespace {

bool isSandboxThread() {
#if defined(CONFIG_APP_EXTENSION_HOST)
    if (extension_host::isCurrentSandboxThread()) {
        return true;
    }
#endif
#if defined(CONFIG_APP_WASM3_MVP)
    if (wasm_mvp_runtime::isCurrentSandboxThread()) {
        return true;
    }
#endif
    return false;
}

}  // namespace

extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf* esf) {
    ARG_UNUSED(esf);
    if (reason != K_ERR_KERNEL_PANIC && isSandboxThread()) {
        LOG_ERR("fault (reason %u) in extension sandbox, aborting only that thread", reason);
        return;
    }

#if !defined(CONFIG_LOG_MODE_MINIMAL)
    log_panic();
#endif
#if defined(CONFIG_APP_COREDUMP_MANAGER)
    if ((CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) == 0) {
        LOG_ERR("Fatal error (reason %u), rebooting to preserve coredump", reason);
        sys_reboot(SYS_REBOOT_COLD);
    }
#endif
    LOG_ERR("Halting system (reason %u)", reason);
    k_fatal_halt(reason);
}
