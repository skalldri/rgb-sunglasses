/*
 * Coverage for stack_watch_policy.h — the two decisions the stack watcher contributes,
 * separated from the SDK measurement so they can be tested without a kernel.
 *
 * The whole value of this module is that it stays quiet on a healthy system and speaks up
 * on a deteriorating one. Both failure directions are silent in the field: a watcher that
 * never warns looks identical to a healthy board until something overflows, and one that
 * warns constantly gets ignored. So these tests pin the speak/stay-quiet decision rather
 * than the formatting.
 */

#include <debug/stack_watch_policy.h>

#include <zephyr/ztest.h>

#define THRESHOLD 80
#define STEP 5

static struct StackWatchState st;

static void reset_before(void *unused)
{
	ARG_UNUSED(unused);
	stack_watch_reset(&st);
}

ZTEST_SUITE(stack_watch_policy, NULL, NULL, reset_before, NULL, NULL);

ZTEST(stack_watch_policy, test_percentage_arithmetic)
{
	zassert_equal(stack_watch_pct(976, 1024), 95);
	zassert_equal(stack_watch_pct(512, 1024), 50);
	zassert_equal(stack_watch_pct(0, 1024), 0);
	/* A zero-size stack must not divide by zero. */
	zassert_equal(stack_watch_pct(100, 0), 0);
}

ZTEST(stack_watch_policy, test_below_threshold_is_silent)
{
	const uint32_t h = stack_watch_hash("charger_status_thread");

	zassert_false(stack_watch_should_warn(&st, h, 79, THRESHOLD, STEP));
	zassert_false(stack_watch_should_warn(&st, h, 0, THRESHOLD, STEP));
}

ZTEST(stack_watch_policy, test_first_crossing_warns_once)
{
	const uint32_t h = stack_watch_hash("charger_status_thread");

	zassert_true(stack_watch_should_warn(&st, h, 85, THRESHOLD, STEP), "first crossing");
	/* Same reading again, and a small rise below the step, must stay quiet — otherwise
	 * this is a log line every interval forever, which is what it exists to avoid. */
	zassert_false(stack_watch_should_warn(&st, h, 85, THRESHOLD, STEP));
	zassert_false(stack_watch_should_warn(&st, h, 88, THRESHOLD, STEP));
}

/* A thread deteriorating toward overflow must keep speaking. Warning only on the first
 * crossing means 82% -> 99% — down to a few bytes spare — produces one mild-sounding line
 * and then silence through the entire dangerous stretch. */
ZTEST(stack_watch_policy, test_escalation_is_reported)
{
	const uint32_t h = stack_watch_hash("charger_status_thread");

	zassert_true(stack_watch_should_warn(&st, h, 82, THRESHOLD, STEP));
	zassert_false(stack_watch_should_warn(&st, h, 84, THRESHOLD, STEP), "below the step");
	zassert_true(stack_watch_should_warn(&st, h, 87, THRESHOLD, STEP), "+5 speaks again");
	zassert_true(stack_watch_should_warn(&st, h, 99, THRESHOLD, STEP), "near overflow");
}

/*
 * THE REGRESSION THIS SUITE EXISTS FOR.
 *
 * The first implementation tracked a single global count of over-threshold threads and
 * warned when it increased, reasoning that stack usage is a high-water mark so the count
 * can only rise. That is false here: the extension sandbox thread is created on load and
 * k_thread_abort()ed on unload, so it leaves the thread list entirely.
 *
 * Sequence: sandbox trips the threshold (count 1, warned) -> extension unloaded, sandbox
 * gone (count 0) -> charger_status_thread genuinely crosses (count 1, NOT > 1) -> silence,
 * permanently. The exact drift #328 exists to catch, missed, with a clean log until the
 * overflow. Per-thread state makes the two threads independent.
 */
ZTEST(stack_watch_policy, test_a_departed_thread_does_not_suppress_another)
{
	const uint32_t sandbox = stack_watch_hash("ext_sandbox");
	const uint32_t charger = stack_watch_hash("charger_status_thread");

	zassert_true(stack_watch_should_warn(&st, sandbox, 90, THRESHOLD, STEP),
		     "sandbox crosses");
	/* Sandbox thread goes away — nothing reports it at all for a while. */
	/* Now a different thread crosses for the first time. It must warn. */
	zassert_true(stack_watch_should_warn(&st, charger, 85, THRESHOLD, STEP),
		     "a different thread crossing must warn even after another already had");
}

/* Threads are told apart by name, so two simultaneously-over threads each get their own
 * first-crossing warning rather than one masking the other. */
ZTEST(stack_watch_policy, test_threads_are_tracked_independently)
{
	const uint32_t a = stack_watch_hash("thread_a");
	const uint32_t b = stack_watch_hash("thread_b");

	zassert_true(stack_watch_should_warn(&st, a, 90, THRESHOLD, STEP));
	zassert_true(stack_watch_should_warn(&st, b, 90, THRESHOLD, STEP));
	zassert_false(stack_watch_should_warn(&st, a, 90, THRESHOLD, STEP));
	zassert_false(stack_watch_should_warn(&st, b, 90, THRESHOLD, STEP));
}

/* Re-arming after a stack is resized must let the next crossing speak; otherwise an
 * operator bumps a stack, sees nothing further, and concludes the detector confirmed the
 * fix when it is merely latched. */
ZTEST(stack_watch_policy, test_reset_rearms)
{
	const uint32_t h = stack_watch_hash("charger_status_thread");

	zassert_true(stack_watch_should_warn(&st, h, 90, THRESHOLD, STEP));
	zassert_false(stack_watch_should_warn(&st, h, 90, THRESHOLD, STEP));

	stack_watch_reset(&st);
	zassert_true(stack_watch_should_warn(&st, h, 90, THRESHOLD, STEP), "reset must re-arm");
}

/* A full table degrades to warning every time rather than going silent. Repeating a
 * warning is an annoyance; dropping one is the failure this module exists to prevent. */
ZTEST(stack_watch_policy, test_full_table_keeps_warning)
{
	for (int i = 0; i < STACK_WATCH_MAX_TRACKED; i++) {
		char name[16];

		snprintk(name, sizeof(name), "t%d", i);
		zassert_true(stack_watch_should_warn(&st, stack_watch_hash(name), 90, THRESHOLD,
						     STEP));
	}

	const uint32_t extra = stack_watch_hash("one_too_many");

	zassert_true(stack_watch_should_warn(&st, extra, 90, THRESHOLD, STEP));
	zassert_true(stack_watch_should_warn(&st, extra, 90, THRESHOLD, STEP),
		     "an untracked thread must keep warning, not fall silent");
}

/* Distinct names must not collide into one entry, or one thread's crossing would suppress
 * another's. Not a proof, but it covers the names this firmware actually runs. */
ZTEST(stack_watch_policy, test_real_thread_names_do_not_collide)
{
	static const char *const names[] = {
		"main",          "idle",       "logging",           "sysworkq",
		"shell_uart",    "bt_thread",  "BT RX WQ",          "BT LW WQ",
		"led_display_thread",          "pattern_controller_thread",
		"charger_status_thread",       "audio_dsp_thread",  "status_led_thread",
		"imu_thread",    "usbd",       "usbd_msc",          "udc_nrfx",
		"coredump_wq",   "persist_wq", "mcuboot_upd_wq",    "tps25750_wq",
		"mbox_wq #0",    "mcumgr smp", "ext_sandbox",       "stack_watch",
	};

	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		for (size_t j = i + 1; j < ARRAY_SIZE(names); j++) {
			zassert_not_equal(stack_watch_hash(names[i]), stack_watch_hash(names[j]),
					  "'%s' and '%s' hash alike", names[i], names[j]);
		}
	}
}
