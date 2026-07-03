/*
 * extension_animation_proxy — the kernel-side BaseAnimation that stands in
 * for a sandboxed extension inside the existing animation machinery (issue
 * #85). The pattern controller ticks it like any built-in animation; it
 * forwards the work to extension_host, which runs the actual extension code
 * on the K_USER sandbox thread with a deadline.
 *
 * Unlike built-ins there is no BaseAnimationTemplate/CRTP here: extension
 * animation IDs are runtime values (kAnimationIdBase + slot), so the proxies
 * are a plain fixed pool indexed by slot.
 */

#include <animations/animation_base.h>
#include <animations/animation_registry.h>
#include <extensions/extension_animation_proxy.h>
#include <extensions/extension_host.h>
#include <fonts/FontAtlas.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <utility>

LOG_MODULE_REGISTER(ext_proxy, LOG_LEVEL_INF);

namespace {

class ExtensionAnimationProxy : public BaseAnimation {
   public:
    void bind(size_t slot) { slot_ = slot; }

    /* Called by animation_registry_init_registered() at boot and by
     * pattern_controller_change_to_animation() on every switch. Sandbox
     * bring-up deliberately happens in setActive(true) instead, so boot-time
     * init of all registered animations doesn't spin up sandboxes. */
    void init() override {}

    void setActive(bool active) override {
        bool running = active;
        if (active) {
            running = extension_host::activate(slot_);
            errorScrollAccumMs_ = 0;
            errorScrollOffset_ = 0;
        } else {
            extension_host::deactivate(slot_);
        }
        /* Mirror the ACTUAL state — not the requested one — to the BLE
         * is-active characteristic: a rejected activation (faulted slot)
         * must read (and notify) as inactive so the app disables the
         * animation. A bring-up that fails later (the load is deferred to
         * the first tick) is reported by the fault path instead. Built-ins
         * mirror the request directly via BaseAnimationTemplate because
         * they cannot fail to start. */
        animation_registry_set_is_active(extension_host::animationId(slot_), running);
    }

    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override {
        if (extension_host::tick(slot_, timeSinceLastTickMs, renderer)) {
            return;
        }
        if (extension_host::isFaulted(slot_)) {
            /* Dead sandbox: scroll a fault banner (GLIM-style) until the
             * user switches animations — the Is Active characteristic has
             * already been notified false by the fault path, so the panel
             * is the only place left to say WHY the animation stopped. */
            renderError(renderer, timeSinceLastTickMs);
        } else {
            /* Merely inactive / activation still pending its first-tick
             * load: render black rather than stale buffer contents. */
            fillBlack(renderer);
        }
    }

   private:
    static void fillBlack(AnimationRenderer &renderer) {
        const size_t w = renderer.displayWidth();
        const size_t h = renderer.displayHeight();
        for (size_t y = 0; y < h; y++) {
            for (size_t x = 0; x < w; x++) {
                renderer.setPixel(x, y, 0, 0, 0);
            }
        }
    }

    /* Scrolling red "FAULT: <name>" banner, same shape as
     * GlimPlayerAnimation::renderError (1 px per kErrorScrollStepMs via an
     * accumulator, FontAtlas glyphs), but looping: the offset resets once
     * the whole message has scrolled off the left edge. */
    void renderError(AnimationRenderer &renderer, size_t timeSinceLastTickMs) {
        fillBlack(renderer);

        char msg[extension_host::kMaxNameLen + 8];
        snprintf(msg, sizeof(msg), "FAULT: %s", extension_host::name(slot_));

        errorScrollAccumMs_ += timeSinceLastTickMs;
        while (errorScrollAccumMs_ >= kErrorScrollStepMs) {
            errorScrollAccumMs_ -= kErrorScrollStepMs;
            errorScrollOffset_--;
        }

        FontAtlas *atlas = FontAtlas::getInstance();
        const size_t charW = FontAtlas::atlasPixelWidthPerChar;
        int32_t totalWidth = 0;
        for (size_t i = 0; msg[i]; i++) {
            totalWidth += static_cast<int32_t>(charW);
        }
        if (errorScrollOffset_ + totalWidth < 0) {
            errorScrollOffset_ = static_cast<int32_t>(renderer.displayWidth());
        }

        for (size_t i = 0; msg[i]; i++) {
            const int32_t charX = errorScrollOffset_ + static_cast<int32_t>(i * charW);
            if (charX + static_cast<int32_t>(charW) < 0 ||
                charX >= static_cast<int32_t>(renderer.displayWidth())) {
                continue;
            }
            atlas->PrintChar(msg[i], [&](size_t fontX, size_t fontY, bool filled) {
                const int32_t screenX = charX + static_cast<int32_t>(fontX);
                if (filled && screenX >= 0 &&
                    screenX < static_cast<int32_t>(renderer.displayWidth()) &&
                    fontY < renderer.displayHeight()) {
                    renderer.setPixel(screenX, fontY, 255, 0, 0);
                }
            });
        }
    }

    static constexpr size_t kErrorScrollStepMs = 50;

    size_t slot_ = 0;
    size_t errorScrollAccumMs_ = 0;
    int32_t errorScrollOffset_ = 0;
};

ExtensionAnimationProxy sProxies[extension_host::kMaxExtensions];

/* animation_registry factories are plain function pointers (no context
 * argument), so each slot needs its own thunk. The table is expanded from
 * kMaxExtensions via index_sequence so a capacity change needs no hand
 * edits here. */
template <size_t N>
BaseAnimation *proxy_factory() {
    return &sProxies[N];
}

template <size_t... I>
constexpr std::array<AnimationInstanceFactory, sizeof...(I)> make_factories(
    std::index_sequence<I...>) {
    return {{proxy_factory<I>...}};
}

constexpr auto kFactories =
    make_factories(std::make_index_sequence<extension_host::kMaxExtensions>{});

}  // namespace

int extension_animation_proxy_register(size_t slot) {
    if (slot >= extension_host::kMaxExtensions) {
        return -EINVAL;
    }
    sProxies[slot].bind(slot);
    int ret = animation_registry_register(extension_host::animationId(slot), kFactories[slot]);
    if (ret != 0) {
        LOG_ERR("registry rejected extension slot %zu: %d", slot, ret);
    }
    return ret;
}
