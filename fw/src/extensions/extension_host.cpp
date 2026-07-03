/*
 * extension_host.cpp — sandboxed LLEXT animation extension host (issue #85).
 *
 * Executes extension code exclusively on one dedicated K_USER thread confined
 * to a per-extension memory domain. The kernel side (this file + the pattern
 * controller proxy) does all privileged work: filesystem loading, domain
 * setup, input snapshots, and framebuffer copy-out. Extension code can only
 * touch its own llext-allocated regions (TEXT/RODATA/DATA/BSS partitions,
 * added by llext_add_domain()) plus z_libc_partition (TLS pointer — see the
 * CONFIG_USERSPACE notes in fw/CLAUDE.md for why every user thread needs it).
 *
 * Current stage: the "MPU spike" — an `ext spike <path>` shell command that
 * proves the full chain on hardware (FAT load -> 5-partition domain ->
 * exported function running in user mode) before the tick protocol and
 * pattern-controller integration are built on top.
 */

#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <zephyr/llext/fs_loader.h>
#include <zephyr/llext/llext.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/libc-hooks.h>

LOG_MODULE_REGISTER(ext_host);

namespace {

K_THREAD_STACK_DEFINE(sHostStack, CONFIG_APP_EXT_HOST_STACK_SIZE);
struct k_thread sHostThread;

/* Signalled by the sandboxed thread when the spike entry function returns. */
K_SEM_DEFINE(sSpikeDoneSem, 0, 1);

struct k_mem_domain sSpikeDomain;
bool sSpikeUsed = false;

/* Runs in user mode. p1 is the extension's entry function, resolved kernel-
 * side; the thread just calls it and reports completion. Mirrors the EDK
 * sample's user_function (zephyr/samples/subsys/llext/edk/app/src/main.c). */
void spike_thread_entry(void *p1, void *, void *) {
    auto entry = reinterpret_cast<void (*)(void)>(p1);
    entry();
    k_sem_give(&sSpikeDoneSem);
    /* Nothing left to do; the spike is single-shot until reboot. */
    for (;;) {
        k_sleep(K_FOREVER);
    }
}

int cmd_ext_spike(const struct shell *sh, size_t argc, char **argv) {
    if (argc != 2) {
        shell_error(sh, "Usage: ext spike <path-to-llext>");
        return -EINVAL;
    }
    /* k_mem_domain has no teardown API and the thread/stack are single
     * instances, so the spike is deliberately one-shot per boot. */
    if (sSpikeUsed) {
        shell_error(sh, "spike already ran; reboot to run again");
        return -EBUSY;
    }
    sSpikeUsed = true;

    struct llext_fs_loader fs_loader = LLEXT_FS_LOADER(argv[1]);
    struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;
    struct llext *ext = nullptr;

    int ret = llext_load(&fs_loader.loader, "spike", &ext, &ldr_parm);
    if (ret < 0) {
        shell_error(sh, "llext_load(%s) failed: %d", argv[1], ret);
        return ret;
    }
    shell_print(sh, "loaded %s (heap alloc %zu bytes)", argv[1], ext->alloc_size);

    const void *entry = llext_find_sym(&ext->exp_tab, "spike_entry");
    const void *counter = llext_find_sym(&ext->exp_tab, "spike_runs");
    if (entry == nullptr || counter == nullptr) {
        shell_error(sh, "spike_entry/spike_runs not exported by %s", argv[1]);
        llext_unload(&ext);
        return -ENOENT;
    }

    /* Domain = z_libc_partition + the extension's 4 llext partitions.
     * 5 partitions total is the budget question this spike answers — both
     * calls below fail loudly if the MPU can't fit them. */
    struct k_mem_partition *parts[] = {&z_libc_partition};
    ret = k_mem_domain_init(&sSpikeDomain, ARRAY_SIZE(parts), parts);
    if (ret != 0) {
        shell_error(sh, "k_mem_domain_init failed: %d", ret);
        return ret;
    }
    ret = llext_add_domain(ext, &sSpikeDomain);
    if (ret != 0) {
        shell_error(sh, "llext_add_domain failed: %d", ret);
        return ret;
    }

    k_tid_t tid = k_thread_create(&sHostThread, sHostStack, K_THREAD_STACK_SIZEOF(sHostStack),
                                  spike_thread_entry, const_cast<void *>(entry), nullptr, nullptr,
                                  CONFIG_APP_EXT_HOST_THREAD_PRIORITY, K_FP_REGS | K_USER,
                                  K_FOREVER);
    k_thread_name_set(tid, "ext_host");
    k_thread_access_grant(tid, &sSpikeDoneSem);
    ret = k_mem_domain_add_thread(&sSpikeDomain, tid);
    if (ret != 0) {
        shell_error(sh, "k_mem_domain_add_thread failed: %d", ret);
        k_thread_abort(tid);
        return ret;
    }
    k_thread_start(tid);

    ret = k_sem_take(&sSpikeDoneSem, K_MSEC(1000));
    /* Kernel mode may read the user partition directly. */
    unsigned int runs = *static_cast<const volatile unsigned int *>(counter);
    if (ret == 0) {
        shell_print(sh, "SPIKE OK: entry ran in user mode, spike_runs=%u", runs);
    } else {
        shell_error(sh, "SPIKE FAILED: no completion within 1s (spike_runs=%u)", runs);
    }
    return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_ext, SHELL_CMD_ARG(spike, NULL, "Run MPU/user-mode spike: ext spike <path>", cmd_ext_spike, 2, 0),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ext, &sub_ext, "Animation extension host", NULL);

}  // namespace
