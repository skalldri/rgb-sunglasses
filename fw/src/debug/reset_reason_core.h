#pragma once

/*
 * Pure reset-cause decoding, split out so the native_sim suite
 * (fw/tests/debug/reset_reason) can cover it without hwinfo, a shell or a kernel — the
 * same seam coredump_manager_core.{h,cpp} uses one directory over.
 *
 * The Zephyr glue (capturing RESETREAS at boot, clearing it, logging, the shell command)
 * stays in reset_reason.c. Everything branchy lives here.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a decode attempt produced, so callers pick severity and wording from one place
 * instead of each re-deriving the ladder. */
enum ResetReasonKind {
	/* At least one cause bit was recognised; `out` holds a comma-separated list. */
	RESET_REASON_NAMED = 0,
	/* RESETREAS was zero. NOT necessarily a clean power-on — see the note below. */
	RESET_REASON_NONE,
	/* Non-zero, but no bit matched a known name (a SoC whose driver reports bits we
	 * have no name for). The raw value is the only useful output. */
	RESET_REASON_UNRECOGNISED,
	/* hwinfo could not be read at all; `out` is empty and the cause is unknown. */
	RESET_REASON_UNAVAILABLE,
};

/*
 * Writes a comma-separated list of the causes set in @p cause into @p out.
 *
 * @p captureRc is the errno from the hwinfo read (0 on success); a negative value short
 * circuits to RESET_REASON_UNAVAILABLE so callers do not have to check it separately.
 *
 * Truncation is safe and never produces a half-written name: if a name will not fit, the
 * buffer is terminated at the last complete entry and the list simply ends early. A
 * truncated list still reports RESET_REASON_NAMED, because the causes were recognised —
 * reporting UNRECOGNISED there would blame the SoC for a buffer that was too small.
 *
 * @return the decode outcome; @p out is always NUL-terminated when @p cap > 0.
 */
enum ResetReasonKind reset_reason_describe(int captureRc, uint32_t cause, char *out,
					   size_t cap);

/*
 * True when @p cause contains a bit that means something went wrong, as opposed to an
 * ordinary pin/software/debugger reset. Callers use it to choose warning vs informational
 * severity, so a real fault stands out in a scrollback.
 */
bool reset_reason_is_fault(uint32_t cause);

#ifdef __cplusplus
}
#endif
