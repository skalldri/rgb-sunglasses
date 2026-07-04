/* zephyr/debug/coredump.h has no extern "C" guards of its own; without this
 * wrapper the C-implemented coredump_query()/coredump_cmd() fail to link from
 * C++ (undefined mangled references). */
extern "C" {
#include <zephyr/debug/coredump.h>
}

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "coredump_manager_core.h"

/* Coredump manager (issue #80): periodically drains crash dumps captured in
 * the internal-flash coredump_partition (written by the NCS
 * DEBUG_COREDUMP_BACKEND_NRF_FLASH_PARTITION backend during a fatal fault)
 * into /NAND:/coredump/core_NNNN.bin, and nags over the log until the files
 * are collected (fw/scripts/coredump-fetch.sh) and deleted.
 *
 * The check runs every CONFIG_APP_COREDUMP_REMINDER_PERIOD_S rather than only
 * at boot: extension-sandbox faults are demoted to a thread abort by
 * k_sys_fatal_error_handler (extension_host.cpp) and never reboot, but
 * z_fatal_error() captures their coredump before the handler runs — the
 * periodic pass harvests those dumps while the system keeps running. */

LOG_MODULE_REGISTER(coredump_manager, CONFIG_LOG_DEFAULT_LEVEL);

namespace {

constexpr const char kDumpDir[] = "/NAND:/coredump";

/* Real partition ops: thin wrappers over Zephyr's coredump query/cmd API,
 * matching the PartitionOps seam used by the testable core logic. */
int real_has_dump() {
    return coredump_query(COREDUMP_QUERY_HAS_STORED_DUMP, nullptr);
}

int real_verify() {
    return coredump_cmd(COREDUMP_CMD_VERIFY_STORED_DUMP, nullptr);
}

int real_get_size() {
    return coredump_query(COREDUMP_QUERY_GET_STORED_DUMP_SIZE, nullptr);
}

int real_copy(off_t offset, uint8_t* buffer, size_t length) {
    struct coredump_cmd_copy_arg arg = {
        .offset = offset,
        .buffer = buffer,
        .length = length,
    };
    return coredump_cmd(COREDUMP_CMD_COPY_STORED_DUMP, &arg);
}

int real_invalidate() {
    return coredump_cmd(COREDUMP_CMD_INVALIDATE_STORED_DUMP, nullptr);
}

constexpr coredump_manager_core::PartitionOps kRealOps = {
    .has_dump = real_has_dump,
    .verify = real_verify,
    .get_size = real_get_size,
    .copy = real_copy,
    .invalidate = real_invalidate,
};

/* FAT I/O runs on a dedicated workqueue: FATFS calls are stack-hungry
 * (fatfs reformat measured ~5 KB) — too big for the 2 KB system workqueue.
 * Same pattern as persistent_value_store.cpp. */
K_THREAD_STACK_DEFINE(coredump_workq_stack, CONFIG_APP_COREDUMP_WORKQ_STACK_SIZE);
struct k_work_q coredump_workq;

void check_work_handler(struct k_work* work);
K_WORK_DELAYABLE_DEFINE(sCheckWork, check_work_handler);

void check_work_handler(struct k_work* work) {
    int rc = coredump_manager_core::drain_to_dir(kRealOps, kDumpDir);
    if (rc < 0 && rc != -ENOENT) {
        LOG_WRN("coredump drain failed (%d) — will retry", rc);
    }

    if (coredump_manager_core::any_dump_files(kDumpDir)) {
        LOG_WRN("crash dump(s) awaiting collection in %s — run fw/scripts/coredump-fetch.sh",
                kDumpDir);
    }

    k_work_reschedule_for_queue(&coredump_workq, k_work_delayable_from_work(work),
                                K_SECONDS(CONFIG_APP_COREDUMP_REMINDER_PERIOD_S));
}

int coredump_manager_init() {
    k_work_queue_init(&coredump_workq);
    k_work_queue_start(&coredump_workq, coredump_workq_stack,
                       K_THREAD_STACK_SIZEOF(coredump_workq_stack),
                       (CONFIG_NUM_PREEMPT_PRIORITIES - 1), NULL);
    // First pass shortly after boot (once USB/FAT have settled), then periodic.
    k_work_reschedule_for_queue(&coredump_workq, &sCheckWork, K_SECONDS(5));
    return 0;
}

/* Must init after storage.cpp's mount_fat (CONFIG_APPLICATION_INIT_PRIORITY):
 * the drain work does FAT I/O as soon as the workqueue starts. */
static_assert(CONFIG_APP_COREDUMP_MANAGER_INIT_PRIORITY > CONFIG_APPLICATION_INIT_PRIORITY,
              "coredump manager must initialize after the FAT filesystem mount");

SYS_INIT(coredump_manager_init, APPLICATION, CONFIG_APP_COREDUMP_MANAGER_INIT_PRIORITY);

}  // namespace
