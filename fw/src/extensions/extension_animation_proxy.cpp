/*
 * extension_animation_proxy — the kernel-side BaseAnimation that stands in
 * for a sandboxed extension inside the existing animation machinery (issue
 * #85). The pattern controller ticks it like any built-in animation; it
 * forwards the work to extension_host, which runs the actual extension code
 * on the K_USER sandbox thread with a deadline.
 *
 * Unlike built-ins there is no BaseAnimationTemplate/CRTP here: extension
 * animation IDs are runtime values (0x20 + slot), so the proxies are a plain
 * fixed pool indexed by slot.
 */

#include <animations/animation_base.h>
#include <animations/animation_registry.h>
#include <extensions/extension_animation_proxy.h>
#include <extensions/extension_host.h>
#include <zephyr/logging/log.h>

#include <array>
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
        } else {
            extension_host::deactivate(slot_);
        }
        /* Mirror the ACTUAL state — not the requested one — to the BLE
         * is-active characteristic (PR #89 review finding 3): a failed or
         * rejected sandbox bring-up must read (and notify) as inactive so
         * the app disables the animation instead of showing a live toggle
         * over a black frame. Built-ins mirror the request directly via
         * BaseAnimationTemplate because they cannot fail to start. */
        animation_registry_set_is_active(extension_host::animationId(slot_), running);
    }

    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override {
        if (!extension_host::tick(slot_, timeSinceLastTickMs, renderer)) {
            /* Faulted / hung / inactive extension: render black rather than
             * leaving stale buffer contents on the frame. The fault itself
             * was already logged by extension_host. */
            const size_t w = renderer.displayWidth();
            const size_t h = renderer.displayHeight();
            for (size_t y = 0; y < h; y++) {
                for (size_t x = 0; x < w; x++) {
                    renderer.setPixel(x, y, 0, 0, 0);
                }
            }
        }
    }

   private:
    size_t slot_ = 0;
};

ExtensionAnimationProxy sProxies[extension_host::kMaxExtensions];

/* animation_registry factories are plain function pointers (no context
 * argument), so each slot needs its own thunk. The table is expanded from
 * kMaxExtensions via index_sequence so a capacity change needs no hand
 * edits here (PR #89 review finding 7). */
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

void extension_animation_proxy_register(size_t slot) {
    if (slot >= extension_host::kMaxExtensions) {
        return;
    }
    sProxies[slot].bind(slot);
    int ret = animation_registry_register(extension_host::animationId(slot), kFactories[slot]);
    if (ret != 0) {
        LOG_ERR("registry rejected extension slot %zu: %d", slot, ret);
    }
}
