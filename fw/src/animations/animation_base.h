#pragma once

#include <animations/animation_active_state_observer.h>
#include <animations/animation_renderer.h>

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

    static void registerActiveStateObserver(AnimationActiveStateObserver *observer);

   protected:
    static AnimationActiveStateObserver *sActiveStateObserver_;
};
