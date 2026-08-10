/*
 * Reset-cause reporting (issue #192, unblocks #191).
 *
 * The nRF5340 latches why it last reset in RESETREAS, and nothing was reading it — so an
 * unexplained reboot (issue #191's wedged-GATT churn) left no evidence at all about
 * whether it was a watchdog, a brownout, a software reboot or a CPU lockup. Those have
 * completely different causes, and guessing between them costs far more than the ~300 B
 * this file adds.
 *
 * TWO THINGS HERE ARE DELIBERATE AND EASY TO GET WRONG.
 *
 * 1. The cause is CAPTURED early but LOGGED at APPLICATION level.
 *
 *    Logging is deferred (CONFIG_LOG_MODE_DEFERRED): a message emitted before the USB
 *    CDC backend exists is buffered, and if the buffer fills before a backend drains it
 *    the oldest messages are DROPPED. That is not hypothetical here — a proto0 boot log
 *    captured 2026-08-10 opens with "--- 64 messages dropped ---" followed by
 *    "--- 10 messages dropped ---", losing everything the storage layer logged during
 *    SYS_INIT. A reset cause printed at PRE_KERNEL would be inside that window and would
 *    be exactly the message you most wanted to survive.
 *
 *    So the read happens early (PRE_KERNEL_2, before anything can clear the register)
 *    and the print happens at APPLICATION, by which point the log buffer is draining.
 *
 * 2. The register is CLEARED once read.
 *
 *    RESETREAS is sticky: bits accumulate across resets until explicitly cleared. Left
 *    alone it reports the union of everything that has ever happened to the part, so the
 *    watchdog bit from a fault three weeks ago still reads as set today and every boot
 *    looks like a watchdog boot. Clearing makes each boot report only its own cause.
 *
 *    Consequence worth knowing: `hwinfo reset_cause` from the Zephyr shell will read 0,
 *    because this ran first. Use `reset_cause` (below), which prints the captured value.
 */

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <stdint.h>

LOG_MODULE_REGISTER(reset_reason, CONFIG_LOG_DEFAULT_LEVEL);

/* Captured before anything else can clear RESETREAS; read by the APPLICATION-level
 * report and by the shell command. */
static uint32_t s_reset_cause;
static int s_capture_rc;

struct cause_name {
	uint32_t bit;
	const char *name;
};

/* Ordered most-diagnostic-first: on a part that sets several bits at once, the first
 * match is the one worth putting in front of a reader. */
static const struct cause_name kCauseNames[] = {
	{RESET_WATCHDOG, "watchdog"},
	{RESET_CPU_LOCKUP, "CPU lockup"},
	{RESET_BROWNOUT, "brownout"},
	{RESET_SECURITY, "security violation"},
	{RESET_PARITY, "parity error"},
	{RESET_POR, "power-on"},
	{RESET_PIN, "pin reset"},
	{RESET_SOFTWARE, "software"},
	{RESET_DEBUG, "debugger"},
	{RESET_LOW_POWER_WAKE, "low-power wake"},
	{RESET_TEMPERATURE, "temperature"},
	{RESET_HARDWARE, "hardware"},
	{RESET_USER, "user"},
	{RESET_CLOCK, "clock"},
	{RESET_PLL, "PLL"},
};

/* Writes "watchdog, software" (etc.) into `out`. Returns the number of causes named, so
 * the caller can distinguish "no bits set" from "bits set that we have no name for" —
 * the latter is a real possibility on a SoC whose driver reports vendor-specific bits. */
static size_t format_causes(uint32_t cause, char *out, size_t cap)
{
	size_t named = 0;
	size_t used = 0;

	if (cap == 0) {
		return 0;
	}
	out[0] = '\0';

	for (size_t i = 0; i < ARRAY_SIZE(kCauseNames); i++) {
		if ((cause & kCauseNames[i].bit) == 0) {
			continue;
		}
		const char *sep = (named == 0) ? "" : ", ";
		int written = snprintk(out + used, cap - used, "%s%s", sep, kCauseNames[i].name);
		/* Truncation is not worth failing over — the raw bitmask is always logged
		 * alongside, so a clipped list is still fully recoverable. */
		if (written < 0 || (size_t)written >= cap - used) {
			break;
		}
		used += (size_t)written;
		named++;
	}

	return named;
}

/* PRE_KERNEL_2 rather than PRE_KERNEL_1: hwinfo needs its driver initialised, and
 * nothing between the two levels resets the SoC or touches RESETREAS. */
static int reset_reason_capture(void)
{
	s_capture_rc = hwinfo_get_reset_cause(&s_reset_cause);
	if (s_capture_rc == 0) {
		/* See the file header: sticky bits make every later boot lie. */
		(void)hwinfo_clear_reset_cause();
	}
	return 0;
}

SYS_INIT(reset_reason_capture, PRE_KERNEL_2, 0);

static void report(void)
{
	char names[128];

	if (s_capture_rc == -ENOSYS) {
		LOG_WRN("reset cause unavailable: no hwinfo driver support");
		return;
	}
	if (s_capture_rc < 0) {
		LOG_WRN("reset cause unavailable: hwinfo error %d", s_capture_rc);
		return;
	}
	if (s_reset_cause == 0) {
		/* Genuinely happens: a warm reset that the SoC does not attribute, or a part
		 * whose bits were already cleared by a bootloader stage. Say so plainly
		 * rather than printing an empty cause list. */
		LOG_INF("reset cause: none reported (RESETREAS 0x%08x)", s_reset_cause);
		return;
	}

	const uint32_t raw = s_reset_cause;
	if (format_causes(raw, names, sizeof(names)) == 0) {
		LOG_WRN("reset cause: unrecognised bits (RESETREAS 0x%08x)", raw);
		return;
	}

	/* Warn rather than inform for the causes that mean something went wrong — those are
	 * the ones that should stand out in a scrollback when chasing #191. */
	if ((raw & (RESET_WATCHDOG | RESET_CPU_LOCKUP | RESET_BROWNOUT | RESET_SECURITY |
		    RESET_PARITY)) != 0) {
		LOG_WRN("reset cause: %s (RESETREAS 0x%08x)", names, raw);
	} else {
		LOG_INF("reset cause: %s (RESETREAS 0x%08x)", names, raw);
	}
}

/* APPLICATION so the message lands after the log backend exists — see the file header. */
static int reset_reason_report(void)
{
	report();
	return 0;
}

SYS_INIT(reset_reason_report, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int cmd_reset_cause(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	char names[128];

	if (s_capture_rc < 0) {
		shell_print(sh, "reset cause unavailable (hwinfo error %d)", s_capture_rc);
		return 0;
	}
	shell_print(sh, "RESETREAS at boot: 0x%08x", s_reset_cause);
	if (s_reset_cause == 0) {
		shell_print(sh, "cause: none reported");
	} else if (format_causes(s_reset_cause, names, sizeof(names)) == 0) {
		shell_print(sh, "cause: unrecognised bits");
	} else {
		shell_print(sh, "cause: %s", names);
	}
	/* Pre-empt the obvious next confusion. */
	shell_print(sh, "(`hwinfo reset_cause` reads 0 — this module clears the register at boot)");
	return 0;
}

SHELL_CMD_REGISTER(reset_cause, NULL, "Why the SoC last reset (captured at boot)",
		   cmd_reset_cause);
