#pragma once

#include <animations/animation_types.h>

/**
 * @brief BT-free bridge from the pattern controller to the Core Config
 * "Active Animation" characteristic.
 *
 * pattern_controller_change_to_animation() pushes every activation change
 * (BLE Is Active writes, shell `anim set`, shuffle hops, boot restore) through
 * setLocalActiveAnimation(); the BT side (core_config.cpp) registers the setter
 * at static-init time. This single notification replaces per-animation Is
 * Active notifies, keeping the app's notification-registration count inside
 * Android's ~15-slot budget (BTA_GATTC_NOTIF_REG_MAX).
 *
 * Mirrors AnimationIsActiveBinding's shape minus the activator: the
 * characteristic is read-only, so nothing flows back in this direction.
 *
 * Threading: setLocalActiveAnimation() fires on the caller's thread of
 * pattern_controller_change_to_animation() — BT RX for GATT writes, the shell
 * thread for `anim set`, or the pattern-controller thread for the boot restore
 * and shuffle hops. The registered setter must be safe from all three (a
 * BtGatt characteristic operator= is — same precedent as battery telemetry).
 *
 * The setter registration is a Meyer's singleton written by a plain static-ctor
 * registrar in core_config.cpp, so it is in place before APPLICATION-level
 * SYS_INIT starts any thread (fw/CLAUDE.md "SYS_INIT ordering").
 */
class ActiveAnimationBinding {
   public:
    using SetterCallback = void (*)(Animation);

    static ActiveAnimationBinding *getInstance() {
        static ActiveAnimationBinding instance;
        return &instance;
    }

    /** @brief Registers the BT-side setter (called from core_config.cpp). */
    static void registerSetter(SetterCallback setter) { getInstance()->setter_ = setter; }

    /** @brief Pushes a locally-originated activation change to the BT side. */
    static void setLocalActiveAnimation(Animation animation) {
        if (getInstance()->setter_) {
            getInstance()->setter_(animation);
        }
    }

   private:
    SetterCallback setter_ = nullptr;
};
