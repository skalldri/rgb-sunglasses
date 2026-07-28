#include <animations/animation_registry.h>
#include <animations/animation_renderer.h>
#include <animations/bt_animations.h>
#include <animations/null_animation.h>
#include <bluetooth/boot_gate.h>
#include <configuration_provider.h>
#if defined(CONFIG_STATUS_LED)
#include <status_led/status_led.h>
#endif
#include <core_config.h>
#include <led_controller.h>
#include <pattern_controller.h>
#include <settings/persistent_value_registry.h>
#include <settings/persistent_value_store.h>

#if defined(CONFIG_FAT_FILESYSTEM_ELM)
#include <storage/glim_registry.h>
#endif
#if defined(CONFIG_APP_EXTENSION_HOST)
#include <extensions/extension_host.h>
#include <extensions/extension_limits.h>
#endif
#if defined(CONFIG_APP_SHUFFLE)
#include <animations/shuffle_controller.h>
#include <bluetooth/shuffle_service.h>
#include <zephyr/random/random.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <cstring>

LOG_MODULE_REGISTER(pattern_controller, LOG_LEVEL_INF);

static ConfigurationProvider *sConfigProvider = nullptr;

void pattern_controller_set_config_provider(ConfigurationProvider *provider) {
    sConfigProvider = provider;
}

static ConfigurationProvider &getPatternConfig() {
    if (!sConfigProvider) {
        sConfigProvider = &CoreConfig::getInstance();
    }
    return *sConfigProvider;
}

// PatternControllerBtObserver (the BtStateObserver driving the indicator overlay and
// status LED) lives in src/pattern_controller_bt_observer.cpp so it can be unit-tested
// without linking this TU's render thread / LED controller / FAT / extension host.

void pattern_controller_thread_func(void *a, void *b, void *c);

// Kernel-only thread: K_KERNEL_* skips the 1KB CONFIG_USERSPACE privileged stack; this
// stack can never host a K_USER thread. (Extension code runs on the extension host's own
// K_USER sandbox thread, not this one.)
K_KERNEL_THREAD_DEFINE(pattern_controller_thread, 4096, pattern_controller_thread_func, NULL, NULL,
                       NULL, 6, 0, 0);

// Intentionally unsynchronized: written from the BT thread (via
// PatternControllerBtObserver), the shell thread (`anim indicator ...`) and this file's
// own thread, and read every render tick. Each access is a single aligned enum store or
// load, and every interleaving lands on a valid Indicator value — the worst outcome is
// one frame rendering the previous overlay. The observer's read-then-clear
// (pattern_controller_bt_observer.cpp) is a check-then-act on this variable for the same
// reason: converging on None/BtAdvertising either way is benign. Don't add a lock here
// without checking the render path's timing budget.
Indicator currentIndicator = Indicator::None;
Animation currentAnimation = Animation::None;

float sBrightnessForFrame = 0.1f;

namespace {

constexpr const char *kLastActiveAnimationKey = "core/last_active_animation";

Animation sLoadedAnimation = Animation::None;
bool sAnimationWasLoaded = false;

void lastActiveAnimationDoLoad(void *, const void *data, size_t len) {
    if (len != sizeof(uint32_t)) {
        return;
    }
    uint32_t raw;
    memcpy(&raw, data, sizeof(raw));
    sLoadedAnimation = static_cast<Animation>(raw);
    sAnimationWasLoaded = true;
}

void lastActiveAnimationDoSave(void *) {
    uint32_t raw = static_cast<uint32_t>(currentAnimation);
    persistent_value_store::save_value(kLastActiveAnimationKey, &raw, sizeof(raw));
}

// Caller-owned registry storage (see persistent_value_registry.h) - a file-scope static,
// since this value has no natural per-value object (its state lives in the file statics
// above). Zero flash when persistence is off; the registration below is dead-code-
// eliminated via IS_ENABLED when CONFIG_APP_PERSIST_BT_CONFIG=n.
PersistentValueRegistryEntry sLastActiveAnimationEntry{};

struct LastActiveAnimationRegistrar {
    LastActiveAnimationRegistrar() {
        // Skipped entirely (doLoad/doSave become unreferenced and get linked out) when
        // CONFIG_APP_PERSIST_BT_CONFIG=n, e.g. on the legacy DK board (dk-support
        // branch) - see fw/Kconfig.
        // Failures are logged inside persistent_value_registry_register() itself.
        if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_registry_register(&sLastActiveAnimationEntry,
                                               kLastActiveAnimationKey, nullptr,
                                               lastActiveAnimationDoLoad,
                                               lastActiveAnimationDoSave);
        }
    }
};
[[maybe_unused]] LastActiveAnimationRegistrar sLastActiveAnimationRegistrar;

#if defined(CONFIG_APP_SHUFFLE)

// Production adapters for the ShuffleController seams (issue #121): the pool is the
// animation registry (which already contains extension slots), the config is the
// dedicated Shuffle service's characteristics (issue #243, previously Core Config).
class RegistryShufflePool : public ShuffleAnimationPool {
   public:
    size_t count() override { return animation_registry_count(); }
    Animation idAt(size_t index) override { return animation_registry_id_at(index); }
    bool isEligible(Animation id) override {
        if (id == Animation::None) {
            return false;  // "all LEDs off" is not a show entry
        }
#if defined(CONFIG_APP_EXTENSION_HOST)
        const uint32_t v = static_cast<uint32_t>(id);
        if (v >= extension_host::kAnimationIdBase) {
            const size_t slot = v - extension_host::kAnimationIdBase;
            if (extension_host::isFaulted(slot)) {
                return false;  // never shuffle INTO a faulted extension (escape is separate)
            }
            // Extensions consult the host directly (same precedent as isFaulted) rather
            // than a registry getter — avoids another post-proxy registration whose
            // ignored failure would be the PR #89 silent-miss class.
            return extension_host::shuffleIncluded(slot);
        }
#endif
        // Built-ins: the per-animation "Include in Shuffle" characteristic, pulled
        // through the BT-free registry binding (issue #243). Default true.
        return animation_registry_shuffle_included(id);
    }
};

class ShuffleServiceConfigSource : public ShuffleConfigSource {
   public:
    bool enabled() override { return shuffle_service_get_enabled(); }
    uint32_t minDurationS() override { return shuffle_service_get_min_duration_s(); }
    uint32_t maxDurationS() override { return shuffle_service_get_max_duration_s(); }
};

RegistryShufflePool sShufflePool;
ShuffleServiceConfigSource sShuffleConfigSource;
// sys_rand32_get is entropy-seeded on proto0 (same source matrix_code_animation uses).
ShuffleController sShuffleController(sShufflePool, sShuffleConfigSource, sys_rand32_get,
                                     static_cast<uint64_t>(CONFIG_APP_SHUFFLE_GRACE_S) * 1000u);

#endif  // CONFIG_APP_SHUFFLE

}  // namespace

BaseAnimation *getIndicator(Indicator indicator) {
    switch (indicator) {
        case Indicator::BtConnecting:
            return BtConnectingAnimation::getInstance();

        case Indicator::BtAdvertising:
            return BtAdvertisingAnimation::getInstance();

        case Indicator::BtPairing:
            return BtPairingAnimation::getInstance();

        case Indicator::ExtensionsLoading:
            return BtExtensionsLoadingAnimation::getInstance();

        case Indicator::None:
            // Explicit fallthrough to get to the NULL animation
            break;
    }

    return NULL;
}

BaseAnimation *getAnimation(Animation animation) {
    return animation_registry_get(animation);
}

BaseAnimation *getBestRenderAnimation() {
    // Indicators have priority over other animation types
    if (currentIndicator != Indicator::None) {
        return getIndicator(currentIndicator);
    }

    return getAnimation(currentAnimation);
}

void pattern_controller_thread_func(void *a, void *b, void *c) {
    int ret;

    // LOG_INF("Pattern control thread start!");

    // Initialize all animations
    BtAdvertisingAnimation::getInstance()->init();
    BtConnectingAnimation::getInstance()->init();
    BtPairingAnimation::getInstance()->init();
    BtExtensionsLoadingAnimation::getInstance()->init();

    // Boot notification (issue #208): show this on the BLE status LED while extension
    // discovery/registration runs below, so it's visible for however long
    // boot_gate_wait_ready() ends up blocking bt_thread's first advertising start.
    // Distinct from every BtStateObserver-driven indicator (all blue) and from the power
    // LED (never blue or violet) by using violet here instead.
#if !defined(CONFIG_STATUS_LED)
    pattern_controller_request_indicator(Indicator::ExtensionsLoading);
#else
    status_led_set(1, StatusIndication::FastBreathing, StatusColor::Violet);
#endif

#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
    // Must run before animation_registry_register_defaults(), which seeds the Glim Player's
    // BLE selection characteristic from whatever this discovers. Gated on the animation itself
    // (not just CONFIG_FAT_FILESYSTEM_ELM) - glim_registry is only used by Glim Player, so a
    // build that enables FAT for some other reason shouldn't pay for this filesystem scan.
    glim_registry::init();
#endif

    ret = animation_registry_register_defaults();
    if (ret) {
        LOG_ERR("Failed to register default animations: %d", ret);
    } else {
#if defined(CONFIG_APP_EXTENSION_HOST)
        // Discover, load, and register sandboxed animation extensions (issue #85).
        // Must run here (kernel-mode thread context): it scans the FAT filesystem
        // (fs_* has no syscall coverage) after the prio-90 SYS_INIT mount, and it
        // registers into the animation registry, so it goes between
        // register_defaults() and init_registered().
        extension_host::init();
#endif
        animation_registry_init_registered();
    }

    // Boot-time work that bt_thread's first advertising start is gated on (issue #208) is
    // done - signal it unconditionally, including the ret-failure path above, so a
    // registration failure here can't also permanently block BLE advertising.
    boot_gate_notify_ready();

    // Resume whatever animation was active before the last power-cycle, if any was
    // persisted and is still registered (e.g. its CONFIG_ANIMATION_* might have been
    // disabled in a later firmware build) - otherwise fall back to ZigZag. Safe to read
    // sLoadedAnimation/sAnimationWasLoaded here with no synchronization: settings_load()
    // (SYS_INIT APPLICATION prio 1, in bluetooth_init) always completes before this
    // K_THREAD_DEFINE thread starts.
    Animation startupAnimation = Animation::ZigZag;
    if (sAnimationWasLoaded && getAnimation(sLoadedAnimation)) {
        startupAnimation = sLoadedAnimation;
    }
    // Restore without scheduling a save — this is a read-back, not a user-initiated change.
    pattern_controller_change_to_animation(startupAnimation, false);

    while (true) {
        int64_t startTicks = k_uptime_ticks();

        float kTargetRenderIntervalMs = getPatternConfig().getRenderRateMs();

        size_t bufferId = 0;
        ret = claimBufferForRender(bufferId);
        if (ret) {
            LOG_ERR("Failed to acquire render buffer!");
        } else {
            BaseAnimation *anim = getBestRenderAnimation();

            // Latch current brightness value from the configuration provider
            sBrightnessForFrame = getPatternConfig().getBrightnessFactor();

            class PatternControllerRenderer : public AnimationRenderer {
                const LedConfig *config_;
                size_t bufferId_;

               public:
                PatternControllerRenderer(const LedConfig *c, size_t b)
                    : config_(c), bufferId_(b) {}
                size_t displayWidth() const override { return config_->displayWidth; }
                size_t displayHeight() const override { return config_->displayHeight; }
                void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
                    pattern_controller_set_pixel_in_framebuffer(config_, x, y, bufferId_, r, g, b);
                }
            };
            PatternControllerRenderer renderer(get_current_led_config(), bufferId);

            if (anim) {
                anim->tick(renderer, kTargetRenderIntervalMs);
            } else {
                // No animation: default to all LEDs off to save power
                BaseAnimation *nullAnimation = animation_registry_get(Animation::None);
                if (nullAnimation) {
                    nullAnimation->tick(renderer, kTargetRenderIntervalMs);
                }
            }

            ret = releaseBufferFromRender(bufferId);
            if (ret) {
                LOG_ERR("Failed to release render buffer!");
            }

#if defined(CONFIG_APP_SHUFFLE)
            // Shuffle step (issue #121): runs on this thread only — the one context
            // where change_to_animation + a lazy extension load are already done from
            // (see the boot restore below the loop head). Skipped while an indicator
            // overlay is active, since the indicator is what's actually ticking then and
            // the selected animation's good-moment flag would be stale.
            if (currentIndicator == Indicator::None) {
                BaseAnimation *shuffleCur = anim;  // == getAnimation(currentAnimation) here
                const ShuffleController::Decision d = sShuffleController.onFrame(
                    currentAnimation, static_cast<uint32_t>(kTargetRenderIntervalMs),
                    shuffleCur ? shuffleCur->isAtGoodSwitchPoint() : true);
                if (d.switchNow) {
                    // persist=false: shuffle hops must not churn settings flash — the
                    // last MANUALLY chosen animation stays the persisted boot animation.
                    pattern_controller_change_to_animation(d.next, false);
                }
            }
#endif
        }

        int64_t endTicks = k_uptime_ticks();
        int64_t updateTicks = endTicks - startTicks;

        float updateTimeS = ((float)updateTicks) / ((float)CONFIG_SYS_CLOCK_TICKS_PER_SEC);
        float updateTimeMs = updateTimeS * 1000.0f;

        if (updateTimeMs > kTargetRenderIntervalMs) {
            LOG_WRN("Render update took >kTargetRenderIntervalMs, cannot keep render rate!");
        } else {
            // Sleep for however much time is left
            k_msleep(kTargetRenderIntervalMs - updateTimeMs);
        }
    }
}

int pattern_controller_request_indicator(Indicator ind) {
    currentIndicator = ind;
    return 0;
}

int pattern_controller_reset_indicator() {
    currentIndicator = Indicator::None;
    return 0;
}

Indicator pattern_controller_get_current_indicator(void) {
    return currentIndicator;
}

Animation pattern_controller_get_current_animation(void) {
    return currentAnimation;
}

int pattern_controller_change_to_animation(Animation animation, bool persist) {
    // Try to get the next animation
    BaseAnimation *next = getAnimation(animation);

    if (!next) {
        LOG_ERR("Cannot change to animation %d", (size_t)animation);
        return -ENOEXEC;  // Bail early: we failed to get a pointer to our next animation
    }

    // Mark the current animation as inactive, if possible
    BaseAnimation *curr = getAnimation(currentAnimation);
    if (curr) {
        curr->setActive(false);
    }

    next->init();
    next->setActive(true);

    currentAnimation = animation;

    if (persist && IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        persistent_value_registry_mark_dirty(kLastActiveAnimationKey);
        persistent_value_store::request_save();
    }

    return 0;
}

int pattern_controller_set_pixel_in_framebuffer(const LedConfig *config, size_t x, size_t y,
                                                size_t bufferId, uint8_t red, uint8_t green,
                                                uint8_t blue) {
    // Scale colors by the current brightness before submitting to the LED controller
    return set_pixel_in_framebuffer(config, x, y, bufferId, ((float)red) * sBrightnessForFrame,
                                    ((float)green) * sBrightnessForFrame,
                                    ((float)blue) * sBrightnessForFrame);
}

#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
#define GLIM_PLAYER_SHELL_SUBCMD \
    , (glim_player, 10,          \
       "Glim Player animation (plays files from /NAND:/glim, see the 'glim' shell command)")
#else
#define GLIM_PLAYER_SHELL_SUBCMD
#endif

#if defined(CONFIG_ANIMATION_MATRIX_CODE)
#define MATRIX_CODE_SHELL_SUBCMD \
    , (matrix_code, 11, "Matrix Code animation (green waterfall dots)")
#else
#define MATRIX_CODE_SHELL_SUBCMD
#endif

#if defined(CONFIG_ANIMATION_TILT)
#define TILT_SHELL_SUBCMD \
    , (tilt, 12, "Tilt animation (hue-shifted rainbow driven by accelerometer)")
#else
#define TILT_SHELL_SUBCMD
#endif

#if defined(CONFIG_ANIMATION_PULSE)
#define PULSE_SHELL_SUBCMD \
    , (pulse, 13, "Pulse animation (whole panel breathes a single configurable color)")
#else
#define PULSE_SHELL_SUBCMD
#endif

#if defined(CONFIG_SHELL)

static int cmd_anim_set(const struct shell *shell, size_t argc, char **argv, void *data) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    Animation animation = static_cast<Animation>(reinterpret_cast<intptr_t>(data));
    int ret = pattern_controller_change_to_animation(animation);
    if (ret) {
        shell_error(shell, "Failed to change animation: %d", ret);
    }
    return ret;
}

static int cmd_anim_get(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    Animation current = pattern_controller_get_current_animation();

    /* Map enum value to a human-readable name for display. */
    const char *name;
    switch (current) {
        case Animation::None:
            name = "none";
            break;
        case Animation::ZigZag:
            name = "zigzag";
            break;
        case Animation::Text:
            name = "text";
            break;
        case Animation::BtAdvertising:
            name = "bt_advertising";
            break;
        case Animation::BtConnecting:
            name = "bt_connecting";
            break;
        case Animation::Rainbow:
            name = "rainbow";
            break;
        case Animation::BtPairing:
            name = "bt_pairing";
            break;
        case Animation::MyEyes:
            name = "my_eyes";
            break;
        case Animation::Beat:
            name = "beat";
            break;
        case Animation::FftBars:
            name = "fft_bars";
            break;
#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
        case Animation::GlimPlayer:
            name = "glim_player";
            break;
#endif
#if defined(CONFIG_ANIMATION_MATRIX_CODE)
        case Animation::MatrixCode:
            name = "matrix_code";
            break;
#endif
#if defined(CONFIG_ANIMATION_TILT)
        case Animation::Tilt:
            name = "tilt";
            break;
#endif
#if defined(CONFIG_ANIMATION_PULSE)
        case Animation::Pulse:
            name = "pulse";
            break;
#endif
        default:
            name = "unknown";
            break;
    }

    shell_print(shell, "%s", name);
    return 0;
}

SHELL_SUBCMD_DICT_SET_CREATE(sub_anim_set, cmd_anim_set, (none, 0, "No animation (all LEDs off)"),
                             (zigzag, 1, "ZigZag animation"), (text, 2, "Text animation"),
                             (rainbow, 5, "Rainbow animation"), (my_eyes, 7, "My Eyes animation"),
                             (beat, 8, "Beat animation (per-band flash on beat detection)"),
                             (fft_bars, 9, "FFT Bars animation (live frequency bar graph)")
                                 GLIM_PLAYER_SHELL_SUBCMD
                                 MATRIX_CODE_SHELL_SUBCMD
                                 TILT_SHELL_SUBCMD
                                 PULSE_SHELL_SUBCMD);

static int cmd_anim_indicator_clear(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    pattern_controller_reset_indicator();
    return 0;
}

static int cmd_anim_indicator_get(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    const char *name;
    switch (pattern_controller_get_current_indicator()) {
        case Indicator::None:
            name = "none";
            break;
        case Indicator::BtAdvertising:
            name = "bt_advertising";
            break;
        case Indicator::BtConnecting:
            name = "bt_connecting";
            break;
        case Indicator::BtPairing:
            name = "bt_pairing";
            break;
        case Indicator::ExtensionsLoading:
            name = "extensions_loading";
            break;
        default:
            name = "unknown";
            break;
    }

    shell_print(shell, "%s", name);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_anim_indicator,
    SHELL_CMD(clear, NULL, "Clear the active indicator and return to the current animation",
              cmd_anim_indicator_clear),
    SHELL_CMD(get, NULL, "Print the active indicator overlay (none if the animation is visible)",
              cmd_anim_indicator_get),
    SHELL_SUBCMD_SET_END);

#if defined(CONFIG_APP_SHUFFLE)

static int cmd_anim_shuffle_status(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(shell, "shuffle: %s, min: %u s, max: %u s, grace: %u s",
                shuffle_service_get_enabled() ? "on" : "off", shuffle_service_get_min_duration_s(),
                shuffle_service_get_max_duration_s(), CONFIG_APP_SHUFFLE_GRACE_S);
    return 0;
}

static int cmd_anim_shuffle_on(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shuffle_service_set_enabled(true);
    shell_print(shell, "shuffle on");
    return 0;
}

static int cmd_anim_shuffle_off(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shuffle_service_set_enabled(false);
    shell_print(shell, "shuffle off");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_anim_shuffle,
    SHELL_CMD(status, NULL, "Print shuffle enable/min/max/grace", cmd_anim_shuffle_status),
    SHELL_CMD(on, NULL, "Enable shuffle mode", cmd_anim_shuffle_on),
    SHELL_CMD(off, NULL, "Disable shuffle mode", cmd_anim_shuffle_off), SHELL_SUBCMD_SET_END);

#define ANIM_SHUFFLE_SHELL_CMD \
    SHELL_CMD(shuffle, &sub_anim_shuffle, "Shuffle mode commands (issue #121)", NULL),
#else
#define ANIM_SHUFFLE_SHELL_CMD
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_anim, SHELL_CMD(set, &sub_anim_set, "Switch to a named animation", NULL),
    SHELL_CMD(get, NULL, "Print the current animation name", cmd_anim_get),
    SHELL_CMD(indicator, &sub_anim_indicator, "Indicator commands", NULL),
    ANIM_SHUFFLE_SHELL_CMD SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(anim, &sub_anim, "Animation commands", NULL);

#endif /* CONFIG_SHELL */