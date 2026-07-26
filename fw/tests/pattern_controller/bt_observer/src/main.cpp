/*
 * Tests for PatternControllerBtObserver (fw/src/pattern_controller_bt_observer.cpp)
 * — the BtStateObserver that drives the panel indicator overlay and the onboard
 * status LED from BT state transitions.
 *
 * Regression home for issue #242: a pairing attempt that ends without reaching
 * CONNECTED (passkey never entered / SMP timeout / peer walks away) used to leave
 * the BtPairing overlay up indefinitely, hiding the active animation and freezing
 * shuffle, because onConnected() was the only path that reset the indicator.
 *
 * pattern_controller.cpp (render thread / LED controller / FAT / extension host),
 * bluetooth.cpp (BT host) and status_led.cpp (LED strip driver) are NOT linked
 * here; their observer-visible surface is faked below, matching each real header's
 * declared signature exactly so the linker resolves the unit's calls into these
 * fakes. bt_animations.h is mocked by include-path precedence (see mocks/).
 *
 * The indicator fake is deliberately STATEFUL (a single shared Indicator value,
 * exactly like the real file-scope `currentIndicator`) rather than a call
 * recorder, so the observer's own read-then-decide logic really executes.
 */

#include <animations/bt_animations.h>
#include <bluetooth/bt_state_observer.h>
#include <pattern_controller.h>
#include <pattern_controller_bt_observer.h>
#if defined(CONFIG_STATUS_LED)
#include <status_led/status_led.h>
#endif
#include <zephyr/ztest.h>

/* ---- Fakes ------------------------------------------------------------- */

namespace {

Indicator sIndicator = Indicator::None;
size_t sResetCount = 0;
size_t sRequestCount = 0;

BtStateObserver *sRegisteredObserver = nullptr;

#if defined(CONFIG_STATUS_LED)
struct StatusLedCall {
    size_t ledIndex;
    StatusIndication indication;
    StatusColor color;
};
StatusLedCall sLastStatusLed = {};
size_t sStatusLedCount = 0;
#endif

void reset_fakes() {
    sIndicator = Indicator::None;
    sResetCount = 0;
    sRequestCount = 0;
#if defined(CONFIG_STATUS_LED)
    sLastStatusLed = {};
    sStatusLedCount = 0;
#endif
    BtPairingAnimation::getInstance()->lastPairingCode = 0;
    BtPairingAnimation::getInstance()->initCount = 0;
}

}  // namespace

int pattern_controller_request_indicator(Indicator ind) {
    sIndicator = ind;
    sRequestCount++;
    return 0;
}

int pattern_controller_reset_indicator() {
    sIndicator = Indicator::None;
    sResetCount++;
    return 0;
}

Indicator pattern_controller_get_current_indicator(void) {
    return sIndicator;
}

void bluetooth_register_state_observer(BtStateObserver *observer) {
    sRegisteredObserver = observer;
}

#if defined(CONFIG_STATUS_LED)
void status_led_set(size_t led_index, StatusIndication indication, StatusColor color) {
    sLastStatusLed = {led_index, indication, color};
    sStatusLedCount++;
}
#endif

/* ---- Tests ------------------------------------------------------------- */

static PatternControllerBtObserver sObserver;

ZTEST_SUITE(pattern_controller_bt_observer, NULL, NULL, NULL, NULL, NULL);

/* The unit's own SYS_INIT hook must have registered an observer at boot -
 * without it every transition below would be dead code in the real firmware. */
ZTEST(pattern_controller_bt_observer, test_observer_registered_at_boot) {
    zassert_not_null(sRegisteredObserver,
                     "SYS_INIT hook did not call bluetooth_register_state_observer()");
}

ZTEST(pattern_controller_bt_observer, test_pairing_code_raises_overlay_and_forwards_code) {
    reset_fakes();

    sObserver.onPairingCodeRequired(330597);

    zassert_equal(sIndicator, Indicator::BtPairing, "pairing must display the BtPairing overlay");
    zassert_equal(BtPairingAnimation::getInstance()->lastPairingCode, 330597u,
                  "passkey must be forwarded to the animation");
    zassert_equal(BtPairingAnimation::getInstance()->initCount, 1,
                  "animation must be re-initialised for each new passkey");
}

/* Issue #242: the failed-pairing path. A pairing attempt raises the overlay,
 * then the connection drops without ever reaching CONNECTED, so the state
 * machine returns to ADVERTISING - at which point the overlay is stale and
 * must be cleared. */
ZTEST(pattern_controller_bt_observer, test_failed_pairing_clears_overlay_on_readvertise) {
    reset_fakes();

    sObserver.onPairingCodeRequired(330597);
    zassert_equal(sIndicator, Indicator::BtPairing, "precondition: overlay is up");

    sObserver.onAdvertisingStarted();

#if defined(CONFIG_STATUS_LED)
    zassert_equal(sIndicator, Indicator::None,
                  "issue #242: stale BtPairing overlay must be cleared on re-advertise");
#else
    /* Without a status LED, advertising has its own panel indicator; the point is
     * that the pairing overlay does not survive, whichever way it goes away. */
    zassert_equal(sIndicator, Indicator::BtAdvertising,
                  "advertising indicator must replace the stale pairing overlay");
#endif
    zassert_equal(sResetCount, 1, "the stale pairing overlay must be reset exactly once");
}

/* The clear is conditional: an unrelated overlay (e.g. ExtensionsLoading during
 * boot, which races advertising start) must not be stomped. */
ZTEST(pattern_controller_bt_observer, test_readvertise_leaves_unrelated_overlay_alone) {
    reset_fakes();
    pattern_controller_request_indicator(Indicator::ExtensionsLoading);
    sResetCount = 0;

    sObserver.onAdvertisingStarted();

    zassert_equal(sResetCount, 0, "a non-pairing overlay must never be reset here");
#if defined(CONFIG_STATUS_LED)
    zassert_equal(sIndicator, Indicator::ExtensionsLoading,
                  "ExtensionsLoading must survive an advertising transition");
#else
    zassert_equal(sIndicator, Indicator::BtAdvertising,
                  "without a status LED, advertising still claims the panel");
#endif
}

/* Re-advertising with nothing up must not churn the indicator either. */
ZTEST(pattern_controller_bt_observer, test_readvertise_with_no_overlay_does_not_reset) {
    reset_fakes();

    sObserver.onAdvertisingStarted();

    zassert_equal(sResetCount, 0, "no overlay was up, so nothing should be reset");
}

/* Pre-existing behavior, guarded against regression: a successful connection
 * clears whatever overlay is up (this was the ONLY clear before #242). */
ZTEST(pattern_controller_bt_observer, test_connected_clears_overlay) {
    reset_fakes();
    sObserver.onPairingCodeRequired(330597);

    sObserver.onConnected();

    zassert_equal(sIndicator, Indicator::None, "connecting successfully clears the overlay");
    zassert_equal(sResetCount, 1, "onConnected must reset the indicator");
}

/* A full successful pairing sequence must leave the passkey up for the whole
 * live attempt - the fix must not clear it early. CONNECTING is entered before
 * the passkey is displayed, so only onConnected() may take it down. */
ZTEST(pattern_controller_bt_observer, test_overlay_survives_the_live_pairing_attempt) {
    reset_fakes();

    sObserver.onConnectingStarted();
    sObserver.onPairingCodeRequired(330597);

    zassert_equal(sIndicator, Indicator::BtPairing,
                  "the passkey must stay visible while pairing is in progress");
    zassert_equal(sResetCount, 0, "nothing may clear the overlay mid-attempt");
}

#if defined(CONFIG_STATUS_LED)
/* On proto0 the status LED carries advertising/connecting/connected state. */
ZTEST(pattern_controller_bt_observer, test_status_led_tracks_bt_state) {
    reset_fakes();

    sObserver.onAdvertisingStarted();
    zassert_equal(sLastStatusLed.indication, StatusIndication::Breathing);
    zassert_equal(sLastStatusLed.color, StatusColor::Blue);
    zassert_equal(sLastStatusLed.ledIndex, 1);

    sObserver.onConnectingStarted();
    zassert_equal(sLastStatusLed.indication, StatusIndication::Blinking);

    sObserver.onConnected();
    zassert_equal(sLastStatusLed.indication, StatusIndication::Solid);

    /* Advertising/connecting must NOT claim the panel on this board - that would
     * hide the active animation whenever the phone is away. */
    zassert_equal(sRequestCount, 0, "no panel indicator may be requested for these states");
}
#endif
