/*
 * Reset-cause reporting (issue #192, unblocks #191).
 *
 * The nRF5340 latches why it last reset in RESETREAS, and nothing was reading it — so an
 * unexplained reboot left no evidence about whether it was a watchdog, a software reboot
 * or a CPU lockup. Those have completely different causes.
 *
 * The decode itself lives in reset_reason_core.c (covered by fw/tests/debug/reset_reason);
 * this file is the Zephyr glue.
 *
 * THREE THINGS HERE ARE DELIBERATE AND EASY TO GET WRONG.
 *
 * 1. The cause is CAPTURED early, REPORTED at APPLICATION, and only CLEARED AFTERWARDS.
 *
 *    The ordering matters more than it looks. RESETREAS is sticky: bits accumulate across
 *    resets until cleared, so leaving it alone means every later boot reports the union of
 *    everything that ever happened to the part. That is why the clear exists.
 *
 *    But the captured value lives only in RAM, so clearing it EARLY throws away the
 *    evidence for any boot that dies before the report runs — a fault in bluetooth_init
 *    (SYS_INIT APPLICATION prio 1), in factory_reset_boot_check (prio 0), or a main-stack
 *    overflow during the corrupt-volume f_mkfs that CONFIG_MAIN_STACK_SIZE is sized
 *    against. Each loop iteration would re-clear, so someone attaching a debugger to a
 *    wedged board hours later reads 0x00000000. The sticky bits would have held the
 *    original cause if this module had done nothing at all, so an early clear is a
 *    REGRESSION against not having the module, in exactly the boot-loop scenario #191 is
 *    about.
 *
 *    Deferring the clear to just after the report keeps both properties: a boot that
 *    reaches APPLICATION reports its own cause and starts the next boot clean, and a boot
 *    that dies before then leaves RESETREAS untouched for the next reader.
 *
 * 2. Logging is deferred (CONFIG_LOG_MODE_DEFERRED) and the USB CDC backend only exists
 *    several seconds into boot, by which point CONFIG_LOG_BUFFER_SIZE has long overflowed
 *    — a proto0 boot log opens with "--- 57 messages dropped ---". So the APPLICATION-level
 *    report is NOT a reliable delivery mechanism; it is a convenience for anyone watching a
 *    live console. The `reset_cause` shell command is the surface that actually works, and
 *    it reads the captured value regardless of whether the log line survived.
 *
 * 3. Zephyr's own CONFIG_HWINFO_SHELL is deliberately NOT enabled. It ships a second copy
 *    of this decode table and a `hwinfo reset_cause` command that would read 0 once this
 *    module has cleared the register — a command guaranteed to mislead. `hwinfo devid`
 *    was the only reason to want it, so that is provided as a subcommand here instead,
 *    keeping exactly one implementation in the image.
 */

#include "reset_reason_core.h"

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

#include <errno.h>
#include <stdint.h>

LOG_MODULE_REGISTER(reset_reason, CONFIG_LOG_DEFAULT_LEVEL);

/* One size for every description buffer in this module. */
#define RESET_REASON_DESC_LEN 128

/* Captured before anything else can clear RESETREAS; read by the APPLICATION-level
 * report and by the shell command. */
static uint32_t s_reset_cause;
static int s_capture_rc;

/* PRE_KERNEL_2 rather than PRE_KERNEL_1: hwinfo needs its driver initialised, and nothing
 * between the two levels resets the SoC or touches RESETREAS. Reads only — see note 1. */
static int reset_reason_capture(void)
{
	s_capture_rc = hwinfo_get_reset_cause(&s_reset_cause);
	return 0;
}

SYS_INIT(reset_reason_capture, PRE_KERNEL_2, 0);

static int reset_reason_report(void)
{
	char names[RESET_REASON_DESC_LEN];
	const enum ResetReasonKind kind =
		reset_reason_describe(s_capture_rc, s_reset_cause, names, sizeof(names));

	switch (kind) {
	case RESET_REASON_UNAVAILABLE:
		LOG_WRN("reset cause unavailable: hwinfo error %d", s_capture_rc);
		break;
	case RESET_REASON_NONE:
		/* WRN, not INF, and the caveat is not padding. On the nRF5340 the hwinfo
		 * driver cannot report a brownout at all (no NRFX_RESET_REASON_HAS_VBUS), so
		 * a brownout lands HERE, indistinguishable from a clean power-on. That is the
		 * failure a battery-powered board browning out under LED current would show,
		 * and reading "none reported" as "not a brownout" would send the reader
		 * somewhere else entirely. */
		LOG_WRN("reset cause: none reported (RESETREAS 0x%08x) — on nRF5340 this is a "
			"power-on OR a brownout; the hwinfo driver cannot tell them apart",
			s_reset_cause);
		break;
	case RESET_REASON_UNRECOGNISED:
		LOG_WRN("reset cause: unrecognised bits (RESETREAS 0x%08x)", s_reset_cause);
		break;
	case RESET_REASON_NAMED:
	default:
		if (reset_reason_is_fault(s_reset_cause)) {
			LOG_WRN("reset cause: %s (RESETREAS 0x%08x)", names, s_reset_cause);
		} else {
			LOG_INF("reset cause: %s (RESETREAS 0x%08x)", names, s_reset_cause);
		}
		break;
	}

	/* Only now — see note 1. Report the failure rather than discarding it: if the clear
	 * does not take, RESETREAS keeps accumulating and every later boot (and the shell
	 * command) reports the union of everything that ever happened. A stale watchdog bit
	 * on a clean boot is worse than no module at all, because the wrong answer carries
	 * the same authority as a right one. */
	if (s_capture_rc == 0) {
		int clear_rc = hwinfo_clear_reset_cause();
		if (clear_rc < 0) {
			LOG_WRN("could not clear RESETREAS (%d) — later boots will report stale, "
				"accumulated causes",
				clear_rc);
		}
	}
	return 0;
}

SYS_INIT(reset_reason_report, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if defined(CONFIG_SHELL)

static int cmd_reset_cause(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	char names[RESET_REASON_DESC_LEN];
	const enum ResetReasonKind kind =
		reset_reason_describe(s_capture_rc, s_reset_cause, names, sizeof(names));

	if (kind == RESET_REASON_UNAVAILABLE) {
		shell_print(sh, "reset cause unavailable (hwinfo error %d)", s_capture_rc);
		return 0;
	}

	shell_print(sh, "RESETREAS at boot: 0x%08x", s_reset_cause);
	switch (kind) {
	case RESET_REASON_NONE:
		shell_print(sh, "cause: none reported");
		shell_print(sh, "  (on nRF5340 a brownout also lands here — the hwinfo driver");
		shell_print(sh, "   cannot distinguish it from a power-on)");
		break;
	case RESET_REASON_UNRECOGNISED:
		shell_print(sh, "cause: unrecognised bits");
		break;
	case RESET_REASON_NAMED:
	default:
		shell_print(sh, "cause: %s", names);
		break;
	}
	return 0;
}

static int cmd_reset_devid(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint8_t id[16];
	ssize_t len = hwinfo_get_device_id(id, sizeof(id));

	if (len < 0) {
		shell_error(sh, "hwinfo_get_device_id failed: %d", (int)len);
		return (int)len;
	}
	shell_fprintf(sh, SHELL_NORMAL, "device id: ");
	for (ssize_t i = 0; i < len; i++) {
		shell_fprintf(sh, SHELL_NORMAL, "%02x", id[i]);
	}
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_reset_cause,
	SHELL_CMD(devid, NULL, "Print the SoC's unique device ID", cmd_reset_devid),
	SHELL_SUBCMD_SET_END);

/* Registered as a command with subcommands so `reset_cause` alone still answers the
 * question it names, while `reset_cause devid` covers the one thing CONFIG_HWINFO_SHELL
 * was wanted for (see note 3). */
SHELL_CMD_REGISTER(reset_cause, &sub_reset_cause, "Why the SoC last reset (captured at boot)",
		   cmd_reset_cause);

#endif /* CONFIG_SHELL */
