#include <animations/animation_registry.h>
#include <animations/animation_renderer.h>
#include <animations/bt_animations.h>
#include <animations/null_animation.h>
#include <bluetooth/bt_state_observer.h>
#include <configuration_provider.h>
#if defined(CONFIG_STATUS_LED)
#include <status_led/status_led.h>
#endif
#include <core_config.h>
#include <led_controller.h>
#include <pattern_controller.h>
#include <settings/persistent_value_registry.h>
#include <settings/persistent_value_store.h>
#include <zephyr/init.h>

#if defined(CONFIG_FAT_FILESYSTEM_ELM)
#include <storage/glim_registry.h>
#endif
#if defined(CONFIG_APP_EXTENSION_HOST)
#include <extensions/extension_host.h>
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

class PatternControllerBtObserver : public BtStateObserver {
   public:
    void onAdvertisingStarted() override {
#if !defined(CONFIG_STATUS_LED)
        pattern_controller_request_indicator(Indicator::BtAdvertising);
#else
        status_led_set(1, StatusIndication::Breathing, StatusColor::Blue);
#endif
    }
    void onConnectingStarted() override {
#if !defined(CONFIG_STATUS_LED)
        pattern_controller_request_indicator(Indicator::BtConnecting);
#else
        status_led_set(1, StatusIndication::Blinking, StatusColor::Blue);
#endif
    }
    void onConnected() override {
        pattern_controller_reset_indicator();
#if defined(CONFIG_STATUS_LED)
        status_led_set(1, StatusIndication::Solid, StatusColor::Blue);
#endif
    }
    void onPairingCodeRequired(unsigned int pairingCode) override {
        BtPairingAnimation::getInstance()->init();
        BtPairingAnimation::getInstance()->setPairingCode(pairingCode);
        pattern_controller_request_indicator(Indicator::BtPairing);
#if defined(CONFIG_STATUS_LED)
        status_led_set(1, StatusIndication::Blinking, StatusColor::Blue);
#endif
    }
};

static PatternControllerBtObserver sPatternControllerBtObserver;

static int pattern_controller_register_bt_observer(void) {
    bluetooth_register_state_observer(&sPatternControllerBtObserver);
    return 0;
}
SYS_INIT(pattern_controller_register_bt_observer, APPLICATION, 0);

void pattern_controller_thread_func(void *a, void *b, void *c);

// Kernel-only thread: K_KERNEL_* skips the 1KB CONFIG_USERSPACE privileged stack; this
// stack can never host a K_USER thread. (Extension code runs on the extension host's own
// K_USER sandbox thread, not this one.)
K_KERNEL_THREAD_DEFINE(pattern_controller_thread, 4096, pattern_controller_thread_func, NULL, NULL,
                       NULL, 6, 0, 0);

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
// eliminated on DK via IS_ENABLED.
PersistentValueRegistryEntry sLastActiveAnimationEntry{};

struct LastActiveAnimationRegistrar {
    LastActiveAnimationRegistrar() {
        // Skipped entirely (doLoad/doSave become unreferenced and get linked out) when
        // CONFIG_APP_PERSIST_BT_CONFIG=n, e.g. on rgb_sunglasses_dk - see fw/Kconfig.
        // Failures are logged inside persistent_value_registry_register() itself.
        if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            sLastActiveAnimationEntry.key = kLastActiveAnimationKey;
            sLastActiveAnimationEntry.target = nullptr;
            sLastActiveAnimationEntry.load = lastActiveAnimationDoLoad;
            sLastActiveAnimationEntry.save = lastActiveAnimationDoSave;
            persistent_value_registry_register(&sLastActiveAnimationEntry);
        }
    }
};
[[maybe_unused]] LastActiveAnimationRegistrar sLastActiveAnimationRegistrar;

}  // namespace

BaseAnimation *getIndicator(Indicator indicator) {
    switch (indicator) {
        case Indicator::BtConnecting:
            return BtConnectingAnimation::getInstance();

        case Indicator::BtAdvertising:
            return BtAdvertisingAnimation::getInstance();

        case Indicator::BtPairing:
            return BtPairingAnimation::getInstance();

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
                                 TILT_SHELL_SUBCMD);

static int cmd_anim_indicator_clear(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    pattern_controller_reset_indicator();
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_anim_indicator,
    SHELL_CMD(clear, NULL, "Clear the active indicator and return to the current animation",
              cmd_anim_indicator_clear),
    SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_anim, SHELL_CMD(set, &sub_anim_set, "Switch to a named animation", NULL),
    SHELL_CMD(get, NULL, "Print the current animation name", cmd_anim_get),
    SHELL_CMD(indicator, &sub_anim_indicator, "Indicator commands", NULL), SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(anim, &sub_anim, "Animation commands", NULL);

#endif /* CONFIG_SHELL */