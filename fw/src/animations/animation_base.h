#pragma once

#include <animations/animation_active_state_observer.h>
#include <animations/animation_renderer.h>

#include <cstddef>
#include <cstdint>

// Minimum time a slot-based animation shows one slot before advancing to the next,
// regardless of how fast its content finishes or how small a remotely-writable dwell
// parameter is set (issue #188 follow-up). Each advance calls consumeCurrentAndAdvance(),
// which fires two GATT notifications (up next + now playing) — without this floor a
// degenerate slot (empty text message, dwell time written as 0) would advance every
// render tick and flood the shared BT TX buffer pool. Caps advances at ~2/s, which the
// pool absorbs comfortably. Shared by every slot-cycling animation (Text, My Eyes) so
// the figure can't drift between them if the pool budget ever changes.
inline constexpr size_t kMinSlotDwellMs = 500;

// Per-slot dwell timing for a slot-cycling animation whose boundary IS dwell expiry
// (My Eyes; a future animation with the same shape reuses this rather than hand-rolling
// the accumulate/clamp/boundary logic again). Text does NOT use this: its boundary is
// scroll completion — a function of message length, step time, and display width — and
// its grace request comes from the remaining scroll distance, not a dwell accumulator;
// only the kMinSlotDwellMs floor above is shared with it.
//
// Boundary semantics are deliberately DEFERRED-consume: the tick on which the dwell
// elapses reports a good switch point (isAtBoundary(), grace 0) but the caller must NOT
// consume the queued next slot until the START of the following tick, gated on
// consumePendingAdvance(). Shuffle samples the switch-point flag after tick() and may
// switch animations on exactly that frame — if the boundary tick consumed the slot, a
// switch taken there would eat a slot the user queued via the Up Next characteristic
// (it would notify as "now playing" without ever rendering, and be skipped when the
// animation next runs). Deferring by one render tick (~16 ms, invisible) means a
// switch-away leaves the queue untouched; the queued slot plays on the next activation.
class SlotDwellTracker {
   public:
    // Call at the START of every tick(); true exactly once per elapsed dwell, on the
    // tick AFTER the boundary was reported — the caller consumes the next slot then.
    bool consumePendingAdvance() {
        if (!advancePending_) {
            return false;
        }
        advancePending_ = false;
        elapsedMs_ = 0;
        return true;
    }

    // Call once per tick after consumePendingAdvance(), with this tick's delta and the
    // (remotely-writable, unclamped) requested dwell. A request below kMinSlotDwellMs
    // is clamped here rather than rejected at the GATT write — 0 means "as fast as
    // allowed".
    void accumulate(size_t tickDeltaMs, size_t requestedDwellMs) {
        atBoundary_ = false;
        elapsedMs_ += tickDeltaMs;
        const size_t dwellMs =
            (requestedDwellMs > kMinSlotDwellMs) ? requestedDwellMs : kMinSlotDwellMs;
        if (elapsedMs_ >= dwellMs) {
            atBoundary_ = true;
            advancePending_ = true;
            remainingMs_ = 0;
        } else {
            remainingMs_ = dwellMs - elapsedMs_;
        }
    }

    void reset() {
        elapsedMs_ = 0;
        remainingMs_ = 0;
        atBoundary_ = false;
        advancePending_ = false;
    }

    // True only on the tick whose accumulate() crossed the dwell — the natural
    // boundary shuffle waits for; feed to isAtGoodSwitchPoint().
    bool isAtBoundary() const { return atBoundary_; }

    // How much dwell remains from NOW; feed to goodSwitchPointGraceMs().
    uint32_t remainingMs() const {
        return (remainingMs_ > UINT32_MAX) ? UINT32_MAX : (uint32_t)remainingMs_;
    }

   private:
    size_t elapsedMs_ = 0;
    size_t remainingMs_ = 0;
    bool atBoundary_ = false;
    bool advancePending_ = false;
};

class BaseAnimation {
   public:
    virtual void init() = 0;
    virtual void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) = 0;
    virtual void setActive(bool active) = 0;

    /**
     * @brief True if the most recent tick() ended at a natural boundary where switching
     * to another animation would not look jarring (end of a text scroll, end of a GLIM
     * clip). Sampled by shuffle mode after each tick(), on the same thread that ticked.
     * Default: every frame is a good switch point — most animations (rainbow, tilt,
     * matrix, ...) have no internal narrative to interrupt.
     */
    virtual bool isAtGoodSwitchPoint() const { return true; }

    /**
     * @brief How much longer, from NOW, this animation needs before it reaches its next
     * good switch point. 0 (the default) means "no idea / no request", which leaves
     * shuffle's fixed CONFIG_APP_SHUFFLE_GRACE_S cap in charge exactly as before.
     *
     * Only override this when the answer comes from real state — the GLIM player knows
     * its clip's remaining runtime, the text animation its remaining scroll — so that a
     * long clip or message plays out instead of being hard-cut shortly after shuffle's
     * dwell target. Shuffle treats the value as a REQUEST: it clamps it to
     * CONFIG_APP_SHUFFLE_MAX_GRACE_S and never lets it shorten the fixed cap. Sandboxed
     * extensions have no rgbx hook for this, so they always request 0 and the anti-hang
     * guard covers them unchanged.
     */
    virtual uint32_t goodSwitchPointGraceMs() const { return 0; }

    static void registerActiveStateObserver(AnimationActiveStateObserver *observer);

   protected:
    static AnimationActiveStateObserver *sActiveStateObserver_;
};
