#pragma once
#include <animations/animation_types.h>

class AnimationActivator {
   public:
    virtual ~AnimationActivator() = default;
    virtual void changeToAnimation(Animation id) = 0;

    /**
     * @brief Requests that the given animation be deactivated, if it is currently active.
     *
     * Implementations must ignore this call if `id` is not the currently-active animation,
     * since a remote write of `false` to an already-inactive animation's characteristic is
     * a no-op rather than a request to deactivate whatever else happens to be running.
     */
    virtual void deactivateAnimation(Animation id) = 0;
};
