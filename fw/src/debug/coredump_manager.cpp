/* zephyr/debug/coredump.h has no extern "C" guards of its own; without this
 * wrapper the C-implemented coredump_query()/coredump_cmd() fail to link from
 * C++ (undefined mangled references). */
extern "C" {
#include <zephyr/debug/coredump.h>
}

#include <zephyr/fs/fs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <cerrno>

#include "coredump_manager_core.h"

/* Coredump manager (issue #80): periodically drains crash dumps captured in
 * the internal-flash coredump_partition (written by the NCS
 * DEBUG_COREDUMP_BACKEND_NRF_FLASH_PARTITION backend during a fatal fault)
 * into /NAND:/coredump/core_NNNN.bin. Collect them with
 * fw/scripts/coredump-fetch.sh; check for them on demand with `coredump_mgr status`.
 * There is deliberately no recurring "awaiting collection" log — see the note in
 * check_work_handler().
 *
 * The check runs every CONFIG_APP_COREDUMP_REMINDER_PERIOD_S rather than only
 * at boot: extension-sandbox faults are demoted to a thread abort by
 * k_sys_fatal_error_handler (extension_host.cpp) and never reboot, but
 * z_fatal_error() captures their coredump before the handler runs — the
 * periodic pass harvests those dumps while the system keeps running.
 *
 * THAT PERIOD IS A DATA-LOSS WINDOW, not just a polling interval. The NCS flash
 * backend erases the WHOLE coredump partition at the start of every capture
 * (coredump_flash_backend_start() -> flash_area_flatten(), see
 * subsys/debug/coredump/coredump_backend_flash_partition.c). So the next crash is
 * always captured whether or not the drain has run — what the drain protects is
 * the PREVIOUS dump. A second fault inside this period destroys the first, which
 * on a boot-looping or repeatedly-faulting board is exactly the root-cause dump
 * you wanted. Raising the period is not free. */

/* The log module is REGISTERed once in coredump_manager_core.cpp (which the
 * native_sim test links on its own); this glue file, compiled only into the
 * firmware alongside it, just DECLAREs the same module so both share one log
 * source instead of registering two confusingly-named ones. */
LOG_MODULE_DECLARE(coredump_mgr, CONFIG_LOG_DEFAULT_LEVEL);

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

// FAT filesystem I/O; must never outrank a rendering thread. The default matches
// CONFIG_NUM_PREEMPT_PRIORITIES - 1 (see fw/docs/threading.md).
BUILD_ASSERT(CONFIG_APP_COREDUMP_WORKQ_PRIORITY >= 0 &&
                 CONFIG_APP_COREDUMP_WORKQ_PRIORITY < CONFIG_NUM_PREEMPT_PRIORITIES,
             "CONFIG_APP_COREDUMP_WORKQ_PRIORITY must be a valid preemptible priority");
struct k_work_q coredump_workq;

void check_work_handler(struct k_work* work);
K_WORK_DELAYABLE_DEFINE(sCheckWork, check_work_handler);

void check_work_handler(struct k_work* work) {
    /* Make sure the drain directory exists before anything opens it, probing
     * with fs_stat() first — the only FS call that is silent in BOTH outcomes we
     * hit here. fs_stat() logs nothing on success, and Zephyr's fs_stat()
     * explicitly treats -ENOENT as "a valid stat response" and does not log it
     * either (see subsys/fs/fs.c). The alternatives are not silent: fs_opendir()
     * on a missing /NAND:/coredump (what the status command's scan does) logs an
     * <err> "directory open error (-2)", and an unconditional fs_mkdir() logs
     * "failed to create directory (-17)" once the directory is there. So this
     * stat-then-mkdir stays completely quiet whether the directory already
     * exists (skip mkdir) or is absent (mkdir creates it, silent on success),
     * and only surfaces a log on a genuine, unexpected mkdir failure.
     *
     * DO NOT DELETE THIS AS DEAD CODE. It originally existed to stop
     * the directory scan logging a spurious "-2" every period, and that caller is
     * gone — but it still has a job: drain_to_dir() only mkdirs when a dump
     * actually exists, so on a board that has never crashed this block is the
     * ONLY thing that creates /NAND:/coredump. Without it, `coredump_mgr status` and
     * `fs ls /NAND:/coredump` fail with -ENOENT instead of reporting an empty
     * directory. Runs here, not at init, because FATFS calls are stack-hungry and
     * this workqueue has the larger stack for them. */
    struct fs_dirent dir_stat;
    if (fs_stat(kDumpDir, &dir_stat) == -ENOENT) {
        int mkrc = fs_mkdir(kDumpDir);
        if (mkrc < 0) {
            LOG_WRN("coredump dir %s create failed (%d) — will retry", kDumpDir, mkrc);
        }
    }

    int rc = coredump_manager_core::drain_to_dir(kRealOps, kDumpDir,
                                                 CONFIG_APP_COREDUMP_MAX_FILES);
    /* Warned once per AT-CAP EPISODE, not once per boot. Repeating every period would be
     * the spam the removed "awaiting collection" reminder was deleted for — but latching
     * for the whole boot is wrong too, because this state is explicitly recoverable:
     * collect the files, slots free, the drain resumes. Since sandbox faults never reboot
     * the board, one uptime can run through several fill/collect cycles, and an operator
     * who just cleared the directory would otherwise get silence the next time it filled.
     * So the latch clears on any successful drain. */
    static bool warned_at_cap;
    if (rc == -ENOSPC) {
        if (!warned_at_cap) {
            warned_at_cap = true;
            /* Report the COUNT, not the cap. They differ exactly when it matters — a
             * directory holding more than the cap (e.g. after lowering
             * CONFIG_APP_COREDUMP_MAX_FILES on a board that already had more) would
             * otherwise tell the operator to free one file when they need to free many. */
            int count = 0;
            int maxIndex = -1;
            if (coredump_manager_core::scan_dumps(kDumpDir, &count, &maxIndex) == 0) {
                LOG_WRN("%s holds %d dump(s) of %d (cap) — NEW dumps are being dropped",
                        kDumpDir, count, CONFIG_APP_COREDUMP_MAX_FILES);
            } else {
                LOG_WRN("%s is at the %d-dump cap — NEW dumps are being dropped", kDumpDir,
                        CONFIG_APP_COREDUMP_MAX_FILES);
            }
            LOG_WRN("the oldest are kept (a crash loop's first dump is the useful one); "
                    "collect with coredump-fetch.sh --delete");
        }
    } else {
        if (rc == 0) {
            warned_at_cap = false; /* slots freed and a drain succeeded — re-arm */
        }
        if (rc < 0 && rc != -ENOENT) {
            LOG_WRN("coredump drain failed (%d) — will retry", rc);
        }
    }

    /* No "dumps awaiting collection" reminder. It re-logged every period for as
     * long as any core_*.bin sat on /NAND:, which is until someone runs
     * coredump-fetch.sh — so on a board with an old dump it is a permanent
     * warning every minute, burying real events (the same log-spam reasoning as
     * fw/CLAUDE.md's "no info-level logs in steady-state paths"). It also cost an
     * fs_opendir + readdir sweep of the directory every period purely to decide
     * whether to print it. `coredump_mgr status` answers the same question on
     * demand. The drain above still runs on this period — that is the part that
     * matters — but note what it actually protects: the backend erases the whole
     * partition at the start of every capture, so draining does not enable the
     * next dump, it rescues the PREVIOUS one (see the file header).
     *
     * The tradeoff accepted here: there is no longer any REPEATING announcement
     * that uncollected dumps exist. If a board panics and reboots unattended, the
     * one-shot drain LOG_INF is emitted before anyone attaches a terminal, and a
     * later attach sees a clean log. `coredump_mgr status` (below) is the on-demand
     * replacement for the removed periodic reminder. */

    k_work_reschedule_for_queue(&coredump_workq, k_work_delayable_from_work(work),
                                K_SECONDS(CONFIG_APP_COREDUMP_REMINDER_PERIOD_S));
}

int coredump_manager_init() {
    k_work_queue_init(&coredump_workq);
    // Named so `kernel thread list` can attribute this queue's priority and stack
    // high-water mark on device — see fw/docs/threading.md.
    static const struct k_work_queue_config cfg = {.name = "coredump_wq"};
    k_work_queue_start(&coredump_workq, coredump_workq_stack,
                       K_THREAD_STACK_SIZEOF(coredump_workq_stack),
                       CONFIG_APP_COREDUMP_WORKQ_PRIORITY, &cfg);
    // First pass shortly after boot (once USB/FAT have settled), then periodic.
    k_work_reschedule_for_queue(&coredump_workq, &sCheckWork, K_SECONDS(5));
    return 0;
}

/* Must init after storage.cpp's mount_fat (CONFIG_APPLICATION_INIT_PRIORITY):
 * the drain work does FAT I/O as soon as the workqueue starts. */
static_assert(CONFIG_APP_COREDUMP_MANAGER_INIT_PRIORITY > CONFIG_APPLICATION_INIT_PRIORITY,
              "coredump manager must initialize after the FAT filesystem mount");

SYS_INIT(coredump_manager_init, APPLICATION, CONFIG_APP_COREDUMP_MANAGER_INIT_PRIORITY);

/* `coredump_mgr status` — the on-demand replacement for the removed periodic reminder,
 * for the removed periodic reminder.
 *
 * Deliberately NOT gated on CONFIG_APP_CRASH_TEST_COMMANDS: that symbol guards
 * commands that deliberately crash the firmware, whereas this is read-only and is
 * exactly what you want available on a board that has already crashed. */
/* Prints the dump count against the cap, and how much room is left on the volume they
 * share with extensions and GLIM assets. Both are on demand rather than logged, so the
 * approach-to-full condition is visible without costing a periodic log line. */
static void print_capacity(const struct shell *sh, int count, int scanRc) {
    if (scanRc == 0) {
        shell_print(sh, "files: %d of %d (cap)%s", count, CONFIG_APP_COREDUMP_MAX_FILES,
                    (count >= CONFIG_APP_COREDUMP_MAX_FILES)
                        ? "  <- AT CAP: new dumps are being dropped"
                        : "");
    } else if (scanRc != -ENOENT) {
        /* Say so rather than printing nothing. The same errno is simultaneously making
         * drain_to_dir() abort every pass, so this command — the one an operator runs to
         * investigate dropped dumps — would otherwise hide the cause behind output that
         * merely looks terse. */
        shell_warn(sh, "could not scan %s: %d — dumps are NOT being drained", kDumpDir,
                   scanRc);
    }

    struct fs_statvfs st;
    if (fs_statvfs("/NAND:", &st) == 0) {
        /* f_frsize can be 0 on some backends; SKIP the line rather than print a bogus
         * "0 KB", which reads as a full volume and would send someone deleting extension
         * or GLIM assets that were never the problem. */
        if (st.f_frsize != 0) {
            shell_print(sh, "/NAND: free: %lu KB",
                        (unsigned long)((st.f_bfree * st.f_frsize) / 1024));
        }
    }
}

int cmd_coredump_status(const struct shell *sh, size_t, char **) {
    /* One sweep answers both "are there any" and "how many" — and unlike the old
     * any_dump_files() boolean it can also say "the scan failed", which is the state
     * worth surfacing here. */
    int count = 0;
    int maxIndex = -1;
    const int scanRc = coredump_manager_core::scan_dumps(kDumpDir, &count, &maxIndex);

    if (scanRc == 0 && count > 0) {
        shell_print(sh, "crash dump(s) awaiting collection in %s", kDumpDir);
        print_capacity(sh, count, scanRc);
        shell_print(sh, "collect with: fw/scripts/coredump-fetch.sh [--delete]");
        shell_print(sh, "list with:    fs ls %s", kDumpDir);
        return 0;
    }
    if (scanRc < 0 && scanRc != -ENOENT) {
        print_capacity(sh, count, scanRc);
        return 0;
    }
    /* Distinguish "nothing to collect" from "the drain has not created the
     * directory yet", since the latter looks identical from `fs ls`. */
    struct fs_dirent st;
    if (fs_stat(kDumpDir, &st) == -ENOENT) {
        shell_print(sh, "no dumps (%s does not exist yet)", kDumpDir);
    } else {
        shell_print(sh, "no dumps awaiting collection in %s", kDumpDir);
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_coredump_mgr,
    SHELL_CMD(status, NULL, "Report whether crash dumps are awaiting collection",
              cmd_coredump_status),
    SHELL_SUBCMD_SET_END);

}  // namespace

/* Registered outside the anonymous namespace: SHELL_CMD_REGISTER emits a section
 * symbol that must have external linkage. Named `coredump_mgr` rather than
 * `coredump` because Zephyr's own DEBUG_COREDUMP shell already owns `coredump`
 * (that is where `coredump find` lives) and two roots with the same name silently
 * shadow each other. */
SHELL_CMD_REGISTER(coredump_mgr, &sub_coredump_mgr,
                   "Coredump drain manager (see also Zephyr's own `coredump`)", NULL);
