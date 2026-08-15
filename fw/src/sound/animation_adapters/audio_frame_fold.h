#pragma once

#include <sound/audio_dsp.h>

#include <cstddef>

/* Fold one drained analysis frame into an animation-facing cache: the latest
 * frame wins for every field, but the beat flags OR across all frames drained
 * in ONE tick. At a ~33 ms render tick two ~32 ms analysis frames regularly
 * arrive in the same drain, and plain last-frame-wins silently dropped the
 * older frame's beat — a missed flash for every isBeat() consumer (issue #376).
 *
 * `firstInBatch` must be true for the first frame of a drain batch so the
 * previous tick's flags are cleared; the OR only spans a single batch.
 *
 * Pure and kernel-free on purpose (same testable-seam idiom as
 * led_stats_core / extension_tick_budget): the ztest suite in
 * fw/tests/sound/audio_frame_fold/ exercises it directly.
 */
inline void audio_frame_fold(struct audio_analysis_result &cache,
                             const struct audio_analysis_result &frame, bool firstInBatch) {
    bool beatOr[AUDIO_NUM_BANDS];
    for (size_t b = 0; b < AUDIO_NUM_BANDS; b++) {
        beatOr[b] = (!firstInBatch && cache.beat[b]) || frame.beat[b];
    }
    cache = frame;
    for (size_t b = 0; b < AUDIO_NUM_BANDS; b++) {
        cache.beat[b] = beatOr[b];
    }
}
