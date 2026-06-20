#pragma once

#include <cstddef>

/**
 * @brief Decouples animations from the concrete button subsystem (buttons.h), mirroring how
 * AnimationAudioSource decouples animations from the sound subsystem.
 *
 * Button presses arrive via an ISR-driven callback (ButtonEventListener::onButtonPressed), but
 * an animation's tick() is polled once per frame. update() bridges the two: call it once per
 * tick to snapshot whichever presses have arrived since the previous call, then query that
 * snapshot with wasPressed() for the rest of the tick.
 */
class AnimationButtonSource {
   public:
    virtual ~AnimationButtonSource() = default;

    /** @brief Snapshots and clears pending button presses. Call exactly once per tick. */
    virtual void update() = 0;

    /** @brief True if buttonId was pressed since the last update() call. */
    virtual bool wasPressed(size_t buttonId) const = 0;
};
