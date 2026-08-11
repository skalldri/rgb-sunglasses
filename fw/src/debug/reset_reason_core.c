#include "reset_reason_core.h"

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/*
 * Name table, ordered most-diagnostic-first so the first entry a reader sees on a
 * multi-bit value is the one worth acting on.
 *
 * WHICH OF THESE THE nRF5340 CAN ACTUALLY PRODUCE. zephyr/drivers/hwinfo/hwinfo_nrf.c
 * maps nrfx reset reasons to only PIN, WATCHDOG, CPU_LOCKUP, LOW_POWER_WAKE, DEBUG and
 * SOFTWARE — plus POR only where NRFX_RESET_REASON_HAS_VBUS, which the nRF5340 is not.
 * BROWNOUT, SECURITY and PARITY are therefore unreachable on this SoC. They stay in the
 * table because it costs nothing and keeps the decode correct on a part that does report
 * them, but do NOT read "not a brownout" from their absence — see the RESET_REASON_NONE
 * note in reset_reason.c, which is where a brownout actually lands here.
 */
static const struct {
	uint32_t bit;
	const char *name;
} kCauseNames[] = {
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

bool reset_reason_is_fault(uint32_t cause)
{
	return (cause & (RESET_WATCHDOG | RESET_CPU_LOCKUP | RESET_BROWNOUT | RESET_SECURITY |
			 RESET_PARITY)) != 0;
}

enum ResetReasonKind reset_reason_describe(int captureRc, uint32_t cause, char *out,
					   size_t cap)
{
	if (cap > 0) {
		out[0] = '\0';
	}

	if (captureRc < 0) {
		return RESET_REASON_UNAVAILABLE;
	}
	if (cause == 0) {
		return RESET_REASON_NONE;
	}

	size_t named = 0;
	size_t used = 0;

	for (size_t i = 0; i < ARRAY_SIZE(kCauseNames); i++) {
		if ((cause & kCauseNames[i].bit) == 0) {
			continue;
		}
		if (cap == 0) {
			named++; /* nothing to write into, but the cause is still recognised */
			continue;
		}

		const char *sep = (used == 0) ? "" : ", ";
		int written = snprintf(out + used, cap - used, "%s%s", sep, kCauseNames[i].name);

		if (written < 0 || (size_t)written >= cap - used) {
			/* snprintf has ALREADY written a clipped fragment (and possibly a
			 * dangling separator) into the buffer. Cut it back to the last
			 * complete entry so the list ends cleanly instead of showing half a
			 * word — the caller logs the raw bitmask alongside, so nothing is
			 * actually lost. */
			out[used] = '\0';
			named++; /* recognised, just not printable here */
			break;
		}
		used += (size_t)written;
		named++;
	}

	/* named counts RECOGNISED causes, not printed ones. Truncating on the very first
	 * name must not report UNRECOGNISED — that would blame the SoC for a small buffer. */
	return (named == 0) ? RESET_REASON_UNRECOGNISED : RESET_REASON_NAMED;
}
