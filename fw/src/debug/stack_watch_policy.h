#pragma once

/*
 * Pure decision logic for the stack watcher, split out so the native_sim suite
 * (fw/tests/debug/stack_watch) can cover it without a kernel, a thread analyzer or a
 * shell — the same seam extension_tick_budget.h uses, which fw/CLAUDE.md records as the
 * pattern for exactly this.
 *
 * Only the MEASUREMENT needs the SDK. The two decisions this module actually contributes
 * — "is this thread over the line" and "is this worth saying out loud again" — are
 * integer comparisons, and they are where the bugs were.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Threads tracked for warn-once purposes. Comfortably above the ~25 threads this
 * firmware runs; a table full of entries degrades to "warn every time" for the
 * unlucky extra thread rather than going silent, which is the safe direction. */
#define STACK_WATCH_MAX_TRACKED 32

struct StackWatchEntry {
	uint32_t nameHash;
	uint8_t worstPct;
	bool used;
};

struct StackWatchState {
	struct StackWatchEntry entries[STACK_WATCH_MAX_TRACKED];
};

/* Cheap stable hash of a thread name. Names are compared by hash rather than stored,
 * because the table is per-thread state that must survive threads coming and going. */
static inline uint32_t stack_watch_hash(const char *name)
{
	/* FNV-1a. */
	uint32_t h = 2166136261u;

	if (name == NULL) {
		return 0;
	}
	while (*name != '\0') {
		h ^= (uint8_t)(*name++);
		h *= 16777619u;
	}
	return h;
}

static inline uint8_t stack_watch_pct(size_t used, size_t size)
{
	if (size == 0) {
		return 0;
	}
	return (uint8_t)((used * 100U) / size);
}

static inline void stack_watch_reset(struct StackWatchState *st)
{
	for (size_t i = 0; i < STACK_WATCH_MAX_TRACKED; i++) {
		st->entries[i].used = false;
		st->entries[i].nameHash = 0;
		st->entries[i].worstPct = 0;
	}
}

/*
 * Should this reading be announced?
 *
 * PER-THREAD, deliberately. An earlier version tracked one global count of
 * over-threshold threads and warned when it increased, on the reasoning that stack usage
 * is a high-water mark so the count can only rise. That reasoning is WRONG on this
 * firmware: the extension sandbox thread is created on extension load and
 * k_thread_abort()ed on unload, so it leaves the thread list entirely. A sandbox that
 * transiently tripped the threshold would raise the count to 1, and after it went away a
 * genuine, permanent crossing by another thread would also read 1 — never greater — and
 * would never be reported. The sandbox is also the likeliest thread to trip transiently,
 * since its stack is sized for arbitrary third-party code, so the suppressing event is
 * the common one.
 *
 * ESCALATION IS REPORTED. Warning only on first crossing means a thread going 82% -> 99%
 * — down to a few bytes of spare stack — produces nothing after the first mild-sounding
 * line, then overflows. Each thread's own worst percentage is tracked, and a rise of at
 * least `stepPct` speaks again. That keeps a healthy system silent while letting a
 * deteriorating one stay audible.
 */
static inline bool stack_watch_should_warn(struct StackWatchState *st, uint32_t nameHash,
					   uint8_t pct, uint8_t thresholdPct, uint8_t stepPct)
{
	if (pct < thresholdPct) {
		return false;
	}

	struct StackWatchEntry *free_slot = NULL;

	for (size_t i = 0; i < STACK_WATCH_MAX_TRACKED; i++) {
		struct StackWatchEntry *e = &st->entries[i];

		if (!e->used) {
			if (free_slot == NULL) {
				free_slot = e;
			}
			continue;
		}
		if (e->nameHash != nameHash) {
			continue;
		}
		/* Known thread: speak only on a material rise. */
		if (pct >= e->worstPct + stepPct) {
			e->worstPct = pct;
			return true;
		}
		return false;
	}

	/* First time over the line for this thread. */
	if (free_slot != NULL) {
		free_slot->used = true;
		free_slot->nameHash = nameHash;
		free_slot->worstPct = pct;
	}
	/* Warn even if the table was full — losing the warning is worse than repeating it. */
	return true;
}

#ifdef __cplusplus
}
#endif
