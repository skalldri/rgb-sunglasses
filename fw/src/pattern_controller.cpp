#include <animations/animation_registry.h>
#include <animations/animation_renderer.h>
#include <animations/bt_animations.h>
#include <animations/null_animation.h>
#include <bluetooth/bt_state_observer.h>
#include <configuration_provider.h>
#include <core_config.h>
#include <led_controller.h>
#include <pattern_controller.h>
#include <zephyr/init.h>

#if defined(CONFIG_FAT_FILESYSTEM_ELM)
#include <storage/glim_registry.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

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
        pattern_controller_request_indicator(Indicator::BtAdvertising);
    }
    void onConnectingStarted() override {
        pattern_controller_request_indicator(Indicator::BtConnecting);
    }
    void onConnected() override { pattern_controller_reset_indicator(); }
    void onPairingCodeRequired(unsigned int pairingCode) override {
        BtPairingAnimation::getInstance()->init();
        BtPairingAnimation::getInstance()->setPairingCode(pairingCode);
        pattern_controller_request_indicator(Indicator::BtPairing);
    }
};

static PatternControllerBtObserver sPatternControllerBtObserver;

static int pattern_controller_register_bt_observer(void) {
    bluetooth_register_state_observer(&sPatternControllerBtObserver);
    return 0;
}
SYS_INIT(pattern_controller_register_bt_observer, APPLICATION, 0);

void pattern_controller_thread_func(void *a, void *b, void *c);

K_THREAD_DEFINE(pattern_controller_thread, 4096, pattern_controller_thread_func, NULL, NULL, NULL,
                6, 0, 0);

Indicator currentIndicator = Indicator::None;
Animation currentAnimation = Animation::None;

float sBrightnessForFrame = 0.1f;

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

#if defined(CONFIG_FAT_FILESYSTEM_ELM)
    // Must run before animation_registry_register_defaults(), which seeds the Glim Player's
    // BLE selection characteristic from whatever this discovers.
    glim_registry::init();
#endif

    ret = animation_registry_register_defaults();
    if (ret) {
        LOG_ERR("Failed to register default animations: %d", ret);
    } else {
        animation_registry_init_registered();
    }

    // Start in the ZigZag animation
    pattern_controller_change_to_animation(Animation::ZigZag);

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

int pattern_controller_change_to_animation(Animation animation) {
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
#define GLIM_PLAYER_SHELL_SUBCMD , (glim_player, 10, "Glim Player animation (plays files from /NAND:/glim, see the 'glim' shell command)")
#else
#define GLIM_PLAYER_SHELL_SUBCMD
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
                             GLIM_PLAYER_SHELL_SUBCMD);

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