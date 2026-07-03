#include <buttons.h>
#include <buttons/animation_adapters/button_animation_source.h>

#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
#include <animations/glim_player_animation.h>
#endif

#include <atomic>
#include <cstdint>

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
            pending_.fetch_or(static_cast<uint32_t>(1) << buttonId, std::memory_order_relaxed);
        }
    }

    // Runs on the calling animation's thread (the pattern-controller render thread).
    void update() override {
        // Atomically grab and clear the pending bitmask. snapshot_ itself is only ever
        // written/read from this single thread, so it needs no synchronization.
        snapshot_ = pending_.exchange(0, std::memory_order_relaxed);
    }

    bool wasPressed(size_t buttonId) const override {
        return buttonId < kMaxButtons && (snapshot_ & (static_cast<uint32_t>(1) << buttonId)) != 0;
    }

   private:
    // A plain bool array here would be a data race (undefined behavior in C++, not just a
    // "tick early/late" tolerance issue) since onButtonPressed() (button work-queue thread) and
    // update()/wasPressed() (pattern-controller thread) access it concurrently with no
    // synchronization. An atomic bitmask makes the cross-thread handoff well-defined; a press
    // landing right at a tick boundary may still be seen one tick early/late, which is fine.
    std::atomic<uint32_t> pending_{0};
    uint32_t snapshot_ = 0;
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

#if defined(CONFIG_APP_EXTENSION_HOST)
#include <extensions/extension_host.h>
/* Sandboxed extensions receive the pressed-since-last-tick bitmask via
 * rgbx_inputs.buttons_pressed (the host queries wasPressed() per button each
 * tick — extensions never touch this source directly). Sharing the single
 * always-registered source is safe: only one animation ticks at a time. */
void extension_host_bind_default_button_dependencies() {
    extension_host::set_button_source(&sButtonAnimationSource);
}
#endif
