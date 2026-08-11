/*
 * Thread-stack creep detector (issue #328).
 *
 * WHY THIS EXISTS WHEN CONFIG_THREAD_ANALYZER_AUTO ALREADY DOES SOMETHING SIMILAR.
 *
 * The measurement is entirely Zephyr's: this calls thread_analyzer_run(), which owns the
 * walk and reports each thread's name, stack size and high-water usage. Nothing here
 * re-implements that, and `kernel thread stacks` / thread_analyzer_print() remain the
 * on-demand view.
 *
 * What the SDK does NOT provide is a detector that stays QUIET. THREAD_ANALYZER_AUTO
 * dumps every thread every interval, unconditionally, which on this firmware is a
 * permanent steady-state log — the thing fw/CLAUDE.md forbids ("no info-level logs in
 * steady-state paths ... they become permanent log spam that buries real events"), and
 * the same objection that got the coredump "awaiting collection" reminder deleted.
 *
 * So the policy is the only new thing: warn when a thread FIRST crosses an occupancy
 * threshold, and say nothing at all otherwise.
 *
 * THE PROBLEM THIS SOLVES. charger_status_thread drifted from a documented 912 B / 1024 B
 * (89%) to 976 B (95%, 48 B spare) over months with nobody noticing. Two reasons it was
 * invisible: the growth came from the shared TPS25750/BQ25792 driver helpers rather than
 * from the thread's own file, so nobody editing the cause had reason to look at the
 * effect; and the only detector was a human remembering to open a serial shell. It was
 * eventually found by accident while measuring something else.
 *
 * NO PER-THREAD STATE IS KEPT. Stack usage is a high-water mark, so it only ever rises
 * and a thread that has crossed the threshold stays crossed. That means the COUNT of
 * over-threshold threads is monotonic too, and "count went up" is exactly "a new thread
 * crossed". Tracking one integer therefore gives warn-once-per-thread behaviour without a
 * name table, and the warning can still name every offender because it lists them at the
 * moment it fires.
 */

#include <zephyr/debug/thread_analyzer.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

#include <stdbool.h>
#include <stdint.h>

LOG_MODULE_REGISTER(stack_watch, CONFIG_LOG_DEFAULT_LEVEL);

/* Highest over-threshold count seen so far. Only ever rises, because stack usage is a
 * high-water mark — see the note above. */
static uint32_t s_worst_over_count;

/* Per-run scratch, valid only for the duration of one thread_analyzer_run(). The
 * callback has no user_data parameter, and the run is confined to a single work item on
 * one workqueue, so file-scope scratch is safe here. */
static uint32_t s_run_over_count;
static const struct shell *s_run_shell; /* non-NULL when driven by the shell command */
static bool s_run_shell_printed_any;

static inline bool over_threshold(const struct thread_analyzer_info *info)
{
    if (info->stack_size == 0) {
        return false;
    }
    /* Integer maths, no floats: CONFIG_CBPRINTF_FP_SUPPORT is off in this build and
     * floats are avoided project-wide. */
    return (info->stack_used * 100U) / info->stack_size >= CONFIG_APP_STACK_WATCH_PERCENT;
}

static void count_cb(struct thread_analyzer_info *info)
{
    if (over_threshold(info)) {
        s_run_over_count++;
    }
}

static void report_cb(struct thread_analyzer_info *info)
{
    if (!over_threshold(info)) {
        return;
    }
    const unsigned int pct = (unsigned int)((info->stack_used * 100U) / info->stack_size);
    const size_t spare = info->stack_size - info->stack_used;

    if (s_run_shell != NULL) {
#if defined(CONFIG_SHELL)
        shell_print(s_run_shell, "  %-24s %zu / %zu (%u%%), %zu B spare", info->name,
                    info->stack_used, info->stack_size, pct, spare);
        s_run_shell_printed_any = true;
#endif
    } else {
        /* Spare bytes, not just the percentage: 95% of 1024 is 48 B and 95% of 4096 is
         * 205 B, and the first is a crisis while the second is merely worth watching. */
        LOG_WRN("stack %s at %u%%: %zu / %zu B, %zu B spare", info->name, pct,
                info->stack_used, info->stack_size, spare);
    }
}

/* Runs the analyzer and warns only if MORE threads are over the threshold than the last
 * time it fired. Silent forever on a healthy system. */
static void check(void)
{
    s_run_over_count = 0;
    s_run_shell = NULL;
    thread_analyzer_run(count_cb, 0);

    if (s_run_over_count <= s_worst_over_count) {
        return;
    }
    s_worst_over_count = s_run_over_count;

    LOG_WRN("%u thread stack(s) now at or above %u%% — see fw/docs/threading.md before "
            "resizing, and note the growth may come from a shared driver rather than the "
            "thread's own file",
            s_run_over_count, (unsigned int)CONFIG_APP_STACK_WATCH_PERCENT);
    thread_analyzer_run(report_cb, 0);
}

static void check_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(s_check_work, check_work_handler);

static void check_work_handler(struct k_work *work)
{
    check();
    k_work_reschedule(k_work_delayable_from_work(work),
                      K_SECONDS(CONFIG_APP_STACK_WATCH_INTERVAL_S));
}

static int stack_watch_init(void)
{
    /* Deliberately on the SYSTEM workqueue rather than a dedicated thread: adding a
     * thread (and its stack) to detect stacks growing is a poor trade, and the work is
     * small and infrequent. It is CPU-only — no flash or filesystem I/O — so it does not
     * hit the "never do flash I/O from a cooperative thread" rule that governs sysworkq.
     *
     * CONFIG_THREAD_ANALYZER_RUN_UNLOCKED (default y) keeps the walk from holding
     * interrupts locked across every thread, which the SDK notes is long enough to drop a
     * Bluetooth connection.
     *
     * The first check is delayed a full interval rather than run at boot, for two reasons.
     * Stacks have not reached their high-water marks until the system has done real work,
     * so an early reading is mostly noise. And the warning has to survive: the USB CDC log
     * backend does not attach until seconds into boot, by which point
     * CONFIG_LOG_BUFFER_SIZE has overflowed, so anything logged before then is dropped.
     * Observed directly while testing this with a 10 s interval — the crossing warning
     * fired at ~10 s, the backend attached at ~13 s, and the warning was simply gone.
     * Since the warning fires ONCE per crossing, a dropped one is lost for good; the
     * default 300 s interval keeps the first check well clear of that window.
     *
     * The state is still recoverable regardless: `stack_watch` lists every thread over the
     * threshold on demand, so a lost warning costs the notification, not the information. */
    k_work_reschedule(&s_check_work, K_SECONDS(CONFIG_APP_STACK_WATCH_INTERVAL_S));
    return 0;
}

SYS_INIT(stack_watch_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if defined(CONFIG_SHELL)

static int cmd_stack_watch(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    s_run_shell = sh;
    s_run_shell_printed_any = false;
    shell_print(sh, "threads at or above %u%% of their stack:",
                (unsigned int)CONFIG_APP_STACK_WATCH_PERCENT);
    thread_analyzer_run(report_cb, 0);
    s_run_shell = NULL;

    if (!s_run_shell_printed_any) {
        shell_print(sh, "  (none)");
    }
    /* Point at the full view rather than reprinting it — Zephyr already has one. */
    shell_print(sh, "full per-thread usage: `kernel thread stacks`");
    return 0;
}

/* Re-arms the warning after someone has resized a stack, so the next crossing warns
 * again instead of being suppressed by the old high-water count. */
static int cmd_stack_watch_rearm(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    s_worst_over_count = 0;
    shell_print(sh, "re-armed; the next threshold crossing will warn again");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_stack_watch,
                               SHELL_CMD(rearm, NULL, "Re-arm the threshold warning",
                                         cmd_stack_watch_rearm),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(stack_watch, &sub_stack_watch,
                   "Threads near their stack limit (see also: rearm)", cmd_stack_watch);

#endif /* CONFIG_SHELL */
