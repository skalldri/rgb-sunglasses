/*
 * Thread-stack creep detector (issue #328).
 *
 * WHY THIS EXISTS WHEN CONFIG_THREAD_ANALYZER_AUTO ALREADY DOES SOMETHING SIMILAR.
 *
 * The measurement is entirely Zephyr's: this calls thread_analyzer_run(), which owns the
 * walk and reports each thread's name, stack size and high-water usage. Nothing here
 * re-implements that, and `kernel thread stacks` / thread_analyzer_print() remain the
 * on-demand view. What the SDK does not provide is a detector that stays QUIET:
 * THREAD_ANALYZER_AUTO dumps every thread every interval, which on this firmware is a
 * permanent steady-state log — the thing fw/CLAUDE.md forbids. The stay-quiet policy is
 * the only new thing, and it lives in stack_watch_policy.h so it can be unit-tested.
 *
 * THE PROBLEM THIS SOLVES. charger_status_thread drifted from a documented 912 B / 1024 B
 * (89%) to 976 B (95%, 48 B spare) over months. The growth came from the shared
 * TPS25750/BQ25792 driver helpers rather than the thread's own file, so nobody editing
 * the cause had reason to look at the effect, and the only detector was a human
 * remembering to run `kernel thread stacks`. It was found by accident.
 *
 * KNOWN RESIDUAL RISK, NOT SOLVED HERE. CONFIG_THREAD_ANALYZER_RUN_UNLOCKED (default y)
 * makes the walk k_thread_foreach_unlocked(), whose documentation warns that a thread
 * aborted mid-walk, with its k_thread storage reused, can send the walk down a
 * re-initialised pointer. extension_host.cpp does exactly that: sSandboxThread is a
 * static k_thread that is k_thread_abort()ed on unload and k_thread_create()d again on
 * the next load, so an animation switch landing inside a walk is the hazard. The locked
 * variant is not a free alternative — the SDK notes it holds interrupts long enough to
 * drop a BLE connection, and the scan is milliseconds (see below). Mitigated only by
 * rarity here (a 300 s interval against a walk of a few ms); if this proves to matter,
 * the fix is to serialise against the sandbox lifecycle rather than to switch variants.
 */

#include "stack_watch_policy.h"

#include <zephyr/debug/thread_analyzer.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(stack_watch, CONFIG_LOG_DEFAULT_LEVEL);

/* Percentage points a thread must climb before it is worth mentioning again. */
#define STACK_WATCH_STEP_PCT 5

/* One collected reading. The walk fills a table and the report is emitted AFTER it
 * returns: thread_analyzer_run() gives the callback no user pointer, and printing from
 * inside the callback is what forced the file-scope shell pointer that made the shell
 * command and the periodic check corrupt each other. */
struct Reading {
	char name[24];
	size_t used;
	size_t size;
	uint8_t pct;
};

static struct StackWatchState s_state;

/* Guards s_scratch and the analyzer run itself. Without it the shell thread
 * (preemptible, lowest application priority) and the periodic work can interleave, and
 * the shell command could report "(none)" while a thread sat at 95% — wrong precisely
 * when it is being relied on as the recovery path for a warning lost to the CDC-attach
 * window. */
static K_MUTEX_DEFINE(s_lock);

static struct Reading s_scratch[STACK_WATCH_MAX_TRACKED];
static size_t s_scratch_count;

static void collect_cb(struct thread_analyzer_info *info)
{
	if (s_scratch_count >= STACK_WATCH_MAX_TRACKED || info->stack_size == 0) {
		return;
	}
	struct Reading *r = &s_scratch[s_scratch_count++];

	strncpy(r->name, (info->name != NULL) ? info->name : "?", sizeof(r->name) - 1);
	r->name[sizeof(r->name) - 1] = '\0';
	r->used = info->stack_used;
	r->size = info->stack_size;
	r->pct = stack_watch_pct(info->stack_used, info->stack_size);
}

/* ONE walk. The previous version ran the analyzer twice — once to count, once to print —
 * which doubled the scan cost on the path where it matters most and, because the two
 * walks are not atomic with respect to each other, could report a header count that
 * disagreed with the list beneath it. */
static void collect(void)
{
	s_scratch_count = 0;
	thread_analyzer_run(collect_cb, 0);
}

static void check(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	collect();

	for (size_t i = 0; i < s_scratch_count; i++) {
		const struct Reading *r = &s_scratch[i];

		if (!stack_watch_should_warn(&s_state, stack_watch_hash(r->name), r->pct,
					     CONFIG_APP_STACK_WATCH_PERCENT,
					     STACK_WATCH_STEP_PCT)) {
			continue;
		}
		/* Spare bytes, not just the percentage: 95% of 1024 is 48 B and 95% of 4096
		 * is 205 B, and the first is a crisis while the second is worth watching. */
		LOG_WRN("stack %s at %u%%: %zu / %zu B, %zu B spare — see fw/docs/threading.md",
			r->name, r->pct, r->used, r->size, r->size - r->used);
	}
	k_mutex_unlock(&s_lock);
}

static void check_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(s_check_work, check_work_handler);

/* Own workqueue, PREEMPTIBLE and lowest priority. Not the system workqueue: that is
 * cooperative at -1, which fw/prj.conf and threading.md both call out as the thread
 * nothing can preempt — and this scan is not small. z_stack_space_get() finds the
 * high-water mark by walking the UNTOUCHED part of each stack word by word looking for
 * the fill pattern, so its cost scales with unused space rather than used: main alone is
 * 16 KB mostly unused, and every thread together puts one pass in the ~100 KB range.
 * Blocking the queue that also runs bt_gatt_store_ccc() and the delayed settings work for
 * that long, unpreemptably, is not a trade worth making for a diagnostic. */
K_THREAD_STACK_DEFINE(s_workq_stack, CONFIG_APP_STACK_WATCH_STACK_SIZE);
static struct k_work_q s_workq;

static void check_work_handler(struct k_work *work)
{
	check();
	k_work_reschedule_for_queue(&s_workq, k_work_delayable_from_work(work),
				    K_SECONDS(CONFIG_APP_STACK_WATCH_INTERVAL_S));
}

static int stack_watch_init(void)
{
	static const struct k_work_queue_config cfg = {.name = "stack_watch"};

	k_work_queue_start(&s_workq, s_workq_stack, K_THREAD_STACK_SIZEOF(s_workq_stack),
			   CONFIG_APP_STACK_WATCH_PRIORITY, &cfg);

	/* The first check is delayed a full interval, for two reasons. Stacks have not
	 * reached their high-water marks until the system has done real work, so an early
	 * reading is mostly noise. And the warning has to survive: the USB CDC log backend
	 * does not attach until seconds into boot, by which point CONFIG_LOG_BUFFER_SIZE has
	 * overflowed, so anything logged before then is dropped — observed directly while
	 * testing this with a 10 s interval, where the crossing warning fired at ~10 s, the
	 * backend attached at ~13 s, and the warning was simply gone. The state is still
	 * recoverable via `stack_watch`, so a lost warning costs the notification rather
	 * than the information. */
	k_work_reschedule_for_queue(&s_workq, &s_check_work,
				    K_SECONDS(CONFIG_APP_STACK_WATCH_INTERVAL_S));
	return 0;
}

SYS_INIT(stack_watch_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if defined(CONFIG_SHELL)

static int cmd_stack_watch(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&s_lock, K_FOREVER);
	collect();

	shell_print(sh, "threads at or above %u%% of their stack:",
		    (unsigned int)CONFIG_APP_STACK_WATCH_PERCENT);
	size_t shown = 0;

	for (size_t i = 0; i < s_scratch_count; i++) {
		const struct Reading *r = &s_scratch[i];

		if (r->pct < CONFIG_APP_STACK_WATCH_PERCENT) {
			continue;
		}
		shell_print(sh, "  %-24s %zu / %zu (%u%%), %zu B spare", r->name, r->used,
			    r->size, r->pct, r->size - r->used);
		shown++;
	}
	k_mutex_unlock(&s_lock);

	if (shown == 0) {
		shell_print(sh, "  (none)");
	}
	shell_print(sh, "full per-thread usage: `kernel thread stacks`");
	return 0;
}

/* Re-arms after someone has resized a stack, so the next crossing warns again instead of
 * being suppressed by the recorded high-water percentages. */
static int cmd_stack_watch_rearm(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&s_lock, K_FOREVER);
	stack_watch_reset(&s_state);
	k_mutex_unlock(&s_lock);

	shell_print(sh, "re-armed; the next threshold crossing will warn again");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_stack_watch,
	SHELL_CMD_ARG(rearm, NULL, "Re-arm the threshold warning", cmd_stack_watch_rearm, 1, 0),
	SHELL_SUBCMD_SET_END);

/* SHELL_CMD_ARG_REGISTER, not SHELL_CMD_REGISTER: with mandatory at 0 a mistyped
 * subcommand (`stack_watch rearmm`) falls through to the parent handler, prints a
 * normal-looking listing and returns 0 — so the operator believes they re-armed when the
 * recorded percentages are untouched and the next crossing stays suppressed. */
SHELL_CMD_ARG_REGISTER(stack_watch, &sub_stack_watch,
		       "Threads near their stack limit (see also: rearm)", cmd_stack_watch, 1, 0);

#endif /* CONFIG_SHELL */
