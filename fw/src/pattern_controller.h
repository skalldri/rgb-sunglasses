#pragma once

#include <animations/animation_types.h>
#include <configuration_provider.h>
#include <led_config.h>

enum class Indicator {
    None = 0,
    BtAdvertising,
    BtConnecting,
    BtPairing,
    ExtensionsLoading,
};

int pattern_controller_request_indicator(Indicator ind);

int pattern_controller_reset_indicator();

/**
 * @brief Get the indicator overlay currently rendered over the active animation.
 *
 * @return The active Indicator, or Indicator::None when the animation is visible.
 *
 * Lets a caller clear only its own overlay instead of blindly resetting whatever is up
 * (see PatternControllerBtObserver's stale-pairing clear, issue #242). Also exposed on
 * the shell as `anim indicator get` — `anim get` reports the underlying animation and
 * cannot see the overlay.
 */
Indicator pattern_controller_get_current_indicator(void);

int pattern_controller_change_to_animation(Animation animation, bool persist = true);

Animation pattern_controller_get_current_animation(void);

int pattern_controller_set_pixel_in_framebuffer(const LedConfig* config, size_t x, size_t y,
                                                size_t bufferId, uint8_t red, uint8_t green,
                                                uint8_t blue);

/**
 * @brief Inject a ConfigurationProvider for the pattern controller thread.
 *
 * If not called before the thread reads configuration, CoreConfig::getInstance()
 * is used as the default. Useful for unit tests.
 */
void pattern_controller_set_config_provider(ConfigurationProvider* provider);
