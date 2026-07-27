#pragma once

#include <animations/animation_types.h>

/**
 * @brief Bridges an animation's "include in shuffle" GATT characteristic to the
 * BT-free animation registry (issue #243), mirroring AnimationIsActiveBinding.
 *
 * The BT adapter registers a getter reading its ShuffleIncludeCharacteristic at
 * static-init; animation_registry_register_defaults() later registers
 * @ref included into the registry, which the shuffle pool pulls at pick time.
 * Pull (not push) is deliberate: settings_load() replays persisted values in
 * bluetooth_init(), before the registry is populated on the pattern-controller
 * thread — a push at load time would race and be lost.
 */
template <Animation tAnimationId>
class AnimationShuffleIncludeBinding {
   public:
    using GetterCallback = bool (*)();

    /** @brief Returns the per-animation binding instance. */
    static AnimationShuffleIncludeBinding<tAnimationId> *getInstance() {
        static AnimationShuffleIncludeBinding<tAnimationId> instance;
        return &instance;
    }

    /**
     * @brief Registers the callback that reads the characteristic's current value.
     *
     * @param getter Function returning whether this animation may be shuffled to.
     */
    static void registerGetter(GetterCallback getter) { getInstance()->getter_ = getter; }

    /**
     * @brief Whether shuffle may pick this animation. No registered getter (e.g. a
     * BT-less test build) defaults to true — inclusion is only ever an explicit opt-out.
     */
    static bool included() {
        if (getInstance()->getter_) {
            return getInstance()->getter_();
        }
        return true;
    }

   private:
    GetterCallback getter_ = nullptr;
};
