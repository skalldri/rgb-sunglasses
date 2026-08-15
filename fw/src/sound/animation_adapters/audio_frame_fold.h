#pragma once

#include <sound/audio_dsp.h>

#include <cstddef>

/* Fold one drained analysis frame into an animation-facing cache: the latest
 * frame wins for every field, but the beat flags OR across all frames of one
 * batch. At a ~33 ms render tick two ~32 ms analysis frames regularly arrive
 * together, and plain last-frame-wins silently dropped the older frame's beat
 * — a missed flash for every isBeat() consumer (issue #376).
 *
 * `firstInBatch` must be true for the first frame of a new batch so the
 * previous batch's flags are cleared; the OR only spans a single batch. WHAT a
 * batch is belongs to the caller: the device adapter scopes it to one render
 * tick (inferred by wall clock, since update() runs several times per tick —
 * see SoundAnimationAudioSource::update()); the simulator host scopes it to
 * one tick's catch-up loop (fw/sim/core/host.ts).
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
