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

/**
 * @brief Switch the active animation.
 *
 * Runs SYNCHRONOUSLY on the caller's thread — the cooperative BT RX thread for a
 * GATT write, the shell thread for `anim set`, the SMP workqueue for a FILE_MGMT
 * delete, and the pattern-controller thread for the boot default and shuffle hops.
 * No flash or settings work happens on this path — it used to schedule a settings
 * flush, which cost up to 1.5 s of NVS work per switch (issue #311). Keep it that
 * way; do not add persistence here.
 *
 * That is NOT a blanket "this never blocks" guarantee. For an extension slot,
 * setActive() enters extension_host::activate()/deactivate(), which take the host
 * lock — and tick() holds that same lock across the one-time lazy .llext load
 * (FAT I/O plus relocation, measured ~100 ms). So a GATT Is Active write on the
 * cooperative BT RX thread can still stall there.
 *
 * The selection is NOT persisted; the device boots to the default every time, and
 * an explicit "all animations off" (Animation::None) does not survive a power
 * cycle either — see the note in pattern_controller.cpp.
 *
 * @return 0, or -ENOEXEC if @p animation is not registered (the current animation
 *         is left untouched).
 */
int pattern_controller_change_to_animation(Animation animation);

Animation pattern_controller_get_current_animation(void);

int pattern_controller_set_pixel_in_framebuffer(const LedConfig* config, size_t x, size_t y,
                                                size_t bufferId, uint8_t red, uint8_t green,
                                                uint8_t blue);

/**
 * @brief Render-tick epoch: increments once at the top of every render tick.
 *
 * Lets a per-tick consumer scope work to "this tick" exactly instead of
 * inferring tick boundaries from wall clock — SoundAnimationAudioSource uses it
 * to OR beat flags across every audio frame drained within one tick, however
 * many update() calls that tick makes and however long preemption stretches the
 * gaps between them (PR #378 review: an 8 ms wall-clock window could be split
 * by a cooperative-band preemption, silently dropping a beat). Wraps at 2^32;
 * only equality is ever meaningful. Safe from any thread.
 */
uint32_t pattern_controller_tick_epoch(void);

/**
 * @brief Inject a ConfigurationProvider for the pattern controller thread.
 *
 * If not called before the thread reads configuration, CoreConfig::getInstance()
 * is used as the default. Useful for unit tests.
 */
void pattern_controller_set_config_provider(ConfigurationProvider* provider);
