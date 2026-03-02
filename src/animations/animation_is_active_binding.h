#pragma once

#include <animations/animation_types.h>

#include <pattern_controller.h>

/**
 * @brief Binds an animation's local active-state mirror to remote BLE writes.
 *
 * This singleton-style template stores an optional setter callback used to push
 * local animation-active state changes into a corresponding GATT characteristic,
 * while also exposing @ref onRemoteActiveChange for BLE-originated updates.
 */
template <Animation tAnimationId>
class AnimationIsActiveBinding
{
public:
    using SetterCallback = void (*)(bool);

    /** @brief Returns the per-animation binding instance. */
    static AnimationIsActiveBinding<tAnimationId> *getInstance()
    {
        static AnimationIsActiveBinding<tAnimationId> instance;
        return &instance;
    }

    /**
     * @brief Registers a callback used to update the local characteristic state.
     *
     * @param setter Function invoked by @ref setLocalActiveState.
     */
    static void registerSetter(SetterCallback setter)
    {
        getInstance()->setter_ = setter;
    }

    /**
     * @brief Pushes active-state changes from registry/runtime into BLE state.
     *
     * @param active True if the animation should be marked active locally.
     */
    static void setLocalActiveState(bool active)
    {
        if (getInstance()->setter_)
        {
            getInstance()->setter_(active);
        }
    }

    /**
     * @brief Handles BLE-originated active-state updates.
     *
     * When the remote client writes `true`, the pattern controller switches to
     * this animation.
     *
     * @param active Remote active value received via BLE write.
     */
    static void onRemoteActiveChange(bool active)
    {
        if (active)
        {
            pattern_controller_change_to_animation(tAnimationId);
        }
    }

private:
    SetterCallback setter_ = nullptr;
};
