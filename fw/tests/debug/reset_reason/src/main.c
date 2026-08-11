/*
 * Coverage for reset_reason_core.c — the pure decode behind the `reset_cause` command.
 *
 * The module's value is that a reader can trust what it says about a reboot. Two classes
 * of bug break that without breaking anything visibly: reporting a cause that is wrong,
 * and reporting "I don't recognise this" for a cause that IS recognised. The truncation
 * tests below exist for the second class, which is unreachable on the nRF5340 (at most 6
 * mappable causes against a 128-byte buffer) but fires immediately on a SoC with more or
 * longer names — i.e. during a port, when nobody is looking at this file.
 */

#include <debug/reset_reason_core.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/ztest.h>

#include <string.h>

ZTEST_SUITE(reset_reason_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(reset_reason_tests, test_single_cause_is_named)
{
	char buf[128];
	zassert_equal(reset_reason_describe(0, RESET_PIN, buf, sizeof(buf)),
		      RESET_REASON_NAMED);
	zassert_equal(strcmp(buf, "pin reset"), 0, "got '%s'", buf);
}

/* Multiple bits are listed together, most-diagnostic-first — a watchdog alongside a
 * software reset should lead with the watchdog, because that is the actionable half. */
ZTEST(reset_reason_tests, test_multiple_causes_are_listed_worst_first)
{
	char buf[128];
	zassert_equal(reset_reason_describe(0, RESET_SOFTWARE | RESET_WATCHDOG, buf, sizeof(buf)),
		      RESET_REASON_NAMED);
	zassert_equal(strcmp(buf, "watchdog, software"), 0, "got '%s'", buf);
}

/* Zero is its own outcome, not an error and not an empty list. On the nRF5340 this is
 * also where a brownout lands, which is why callers warn rather than inform. */
ZTEST(reset_reason_tests, test_zero_is_none_not_unrecognised)
{
	char buf[128];
	zassert_equal(reset_reason_describe(0, 0, buf, sizeof(buf)), RESET_REASON_NONE);
	zassert_equal(buf[0], '\0', "buffer should be empty for the none case");
}

/* A bit with no name is honestly unrecognised rather than silently dropped. */
ZTEST(reset_reason_tests, test_unknown_bit_is_unrecognised)
{
	char buf[128];
	zassert_equal(reset_reason_describe(0, 0x80000000u, buf, sizeof(buf)),
		      RESET_REASON_UNRECOGNISED);
}

/* A failed hwinfo read short-circuits, so callers never have to check the rc separately
 * and can never print a cause derived from an uninitialised value. */
ZTEST(reset_reason_tests, test_capture_error_is_unavailable)
{
	char buf[128];
	zassert_equal(reset_reason_describe(-ENOSYS, RESET_PIN, buf, sizeof(buf)),
		      RESET_REASON_UNAVAILABLE);
	zassert_equal(buf[0], '\0', "no cause should be described when the read failed");
}

/* THE TRUNCATION CONTRACT, part 1: the list ends at a complete entry. snprintf has
 * already written a clipped fragment by the time the overflow is detected, so without an
 * explicit cut-back the output shows half a word and a dangling separator. */
ZTEST(reset_reason_tests, test_truncation_leaves_no_partial_name)
{
	/* Room for "watchdog" plus the NUL, but not for ", CPU lockup". */
	char buf[10];
	zassert_equal(reset_reason_describe(0, RESET_WATCHDOG | RESET_CPU_LOCKUP, buf,
					    sizeof(buf)),
		      RESET_REASON_NAMED);
	zassert_equal(strcmp(buf, "watchdog"), 0, "expected a clean cut, got '%s'", buf);
}

/* THE TRUNCATION CONTRACT, part 2: truncating on the FIRST name must still report NAMED.
 * Reporting UNRECOGNISED there blames the SoC for a buffer that was too small, and sends
 * the reader looking for an unknown reset source that does not exist. */
ZTEST(reset_reason_tests, test_truncation_on_first_name_is_still_named)
{
	char buf[4]; /* cannot hold "watchdog" */
	zassert_equal(reset_reason_describe(0, RESET_WATCHDOG, buf, sizeof(buf)),
		      RESET_REASON_NAMED);
	zassert_equal(buf[0], '\0', "a name that does not fit must not be half-written");
}

/* Degenerate buffer: must not write, must not crash, must still classify. */
ZTEST(reset_reason_tests, test_zero_capacity_buffer_is_safe)
{
	char buf[1] = {'x'};
	zassert_equal(reset_reason_describe(0, RESET_WATCHDOG, buf, 0), RESET_REASON_NAMED);
	zassert_equal(buf[0], 'x', "a zero-capacity buffer must not be touched");
}

/* Severity selection: the causes that mean something broke are distinguished from an
 * ordinary pin/software/debugger reset, so a real fault stands out in a scrollback. */
ZTEST(reset_reason_tests, test_fault_classification)
{
	zassert_true(reset_reason_is_fault(RESET_WATCHDOG));
	zassert_true(reset_reason_is_fault(RESET_CPU_LOCKUP));
	zassert_true(reset_reason_is_fault(RESET_BROWNOUT));
	zassert_true(reset_reason_is_fault(RESET_SOFTWARE | RESET_WATCHDOG),
		     "a fault bit alongside a benign one is still a fault");

	zassert_false(reset_reason_is_fault(RESET_PIN));
	zassert_false(reset_reason_is_fault(RESET_SOFTWARE));
	zassert_false(reset_reason_is_fault(RESET_DEBUG));
	zassert_false(reset_reason_is_fault(0));
}
