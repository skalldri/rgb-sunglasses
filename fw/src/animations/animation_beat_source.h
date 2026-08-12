#pragma once

#include <stdint.h>

/**
 * @brief Shared beat feed: a monotonically increasing count of detected beats.
 *
 * Deliberately NOT a consume-once latch (issue #344). A latch is destructive, so with
 * more than one consumer whichever ran first in a tick ate the beat and the rest saw
 * none — and one animation really can hold several independent consumers at once (an
 * extension with two COLOR params on RandomOnBeat, or Pulse's beat-sync envelope
 * alongside a RandomOnBeat colour). Splitting the latch per consumer was tried first
 * and does not scale: the consumer set is not fixed, since an extension may declare up
 * to RGBX_MAX_PARAMS colour params, each with its own resolver.
 *
 * A free-running counter has no such limit. The source counts; each consumer keeps its
 * own cursor (see @ref AnimationBeatCursor) and asks whether the count moved. Every
 * consumer observes every beat, with no registration and no coordination between them.
 *
 * The production implementation counts at audio-queue drain time (see
 * src/sound/animation_adapters/audio_animations_sound.cpp), so it stays immune to which
 * consumer drains the queue first in a tick.
 *
 * Lives in its own header rather than color_mode_source.h so consumers that only need
 * the interface don't compile ColorModeSource and its <atomic> dependency — same split
 * as animations/animation_audio_source.h.
 */
class AnimationBeatSource {
   public:
    virtual ~AnimationBeatSource() = default;

    /**
     * @brief Total beats counted since boot. Wraps at UINT32_MAX, which is harmless:
     * cursors compare for inequality, never ordering.
     * @return Current beat count.
     */
    virtual uint32_t beatCount() = 0;
};

/**
 * @brief One consumer's view of a shared @ref AnimationBeatSource.
 *
 * Each independent consumer owns a cursor. Non-virtual and trivially copyable — it is
 * four bytes of state, intended to be embedded as a member.
 */
class AnimationBeatCursor {
   public:
    /**
     * @brief Whether at least one beat was counted since the previous call.
     * @param source Shared beat source to read.
     * @return true iff the source's count moved since this cursor last looked.
     */
    bool consumeBeat(AnimationBeatSource &source) {
        const uint32_t now = source.beatCount();
        const bool beat = (now != lastSeen_);
        lastSeen_ = now;
        return beat;
    }

    /**
     * @brief Discard every beat counted so far without reporting it.
     *
     * For entering a beat-driven mode: beats counted while the consumer was inactive
     * are stale, and reporting them fires the effect immediately on entry (for Pulse,
     * a full-brightness flash in a silent room).
     *
     * @param source Shared beat source to resynchronise against.
     */
    void resync(AnimationBeatSource &source) { lastSeen_ = source.beatCount(); }

   private:
    uint32_t lastSeen_ = 0;
};
