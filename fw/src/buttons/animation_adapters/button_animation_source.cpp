#include <buttons.h>
#include <buttons/animation_adapters/button_animation_source.h>

#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
#include <animations/glim_player_animation.h>
#endif

#include <algorithm>
#include <iterator>

namespace {
constexpr size_t kMaxButtons = 5;  // sw0-sw3 (0-3) + wake button (4)

/**
 * @brief Single, always-registered AnimationButtonSource/ButtonEventListener.
 *
 * buttons_register_listener() only supports one listener at a time, so rather than have each
 * animation that wants button input register/unregister itself on setActive() (fragile the
 * moment a second animation also wants buttons), this is the one and only listener, registered
 * once for the firmware's lifetime. Any animation that wants button input is injected a
 * reference to this same instance (see button_animation_source()); since only the currently
 * active animation's tick() ever runs, sharing one source is safe.
 */
class ButtonAnimationSource : public AnimationButtonSource, public ButtonEventListener {
   public:
    // Runs on the button work-queue thread (see buttons.cpp).
    void onButtonPressed(size_t buttonId) override {
        if (buttonId < kMaxButtons) {
            pending_[buttonId] = true;
        }
    }

    // Runs on the calling animation's thread (the pattern-controller render thread).
    void update() override {
        std::copy(std::begin(pending_), std::end(pending_), std::begin(snapshot_));
        std::fill(std::begin(pending_), std::end(pending_), false);
    }

    bool wasPressed(size_t buttonId) const override {
        return buttonId < kMaxButtons && snapshot_[buttonId];
    }

   private:
    // No lock: pending_/snapshot_ are plain bools written from one thread and read from
    // another. A press landing right at a tick boundary may be seen one tick early/late, the
    // same small cross-thread tolerance already accepted elsewhere in this codebase (e.g. BT
    // characteristic-backed animation parameters read directly from tick()).
    bool pending_[kMaxButtons] = {};
    bool snapshot_[kMaxButtons] = {};
};

ButtonAnimationSource sButtonAnimationSource;

struct ButtonAnimationSourceRegistrar {
    ButtonAnimationSourceRegistrar() { buttons_register_listener(&sButtonAnimationSource); }
};

[[maybe_unused]] ButtonAnimationSourceRegistrar sButtonAnimationSourceRegistrar;
}  // namespace

AnimationButtonSource &button_animation_source() {
    return sButtonAnimationSource;
}

#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
void glim_player_animation_bind_default_button_dependencies() {
    GlimPlayerAnimation::getInstance()->setButtonSource(button_animation_source());
}
#endif
