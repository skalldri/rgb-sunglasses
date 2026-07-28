#pragma once

#include <animations/animation_active_state_observer.h>
#include <animations/animation_renderer.h>

#include <cstdint>

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
