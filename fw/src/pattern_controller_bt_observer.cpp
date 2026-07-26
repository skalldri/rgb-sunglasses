#include <pattern_controller_bt_observer.h>

#include <animations/bt_animations.h>
#include <pattern_controller.h>
#if defined(CONFIG_STATUS_LED)
#include <status_led/status_led.h>
#endif
#include <zephyr/init.h>

void PatternControllerBtObserver::onAdvertisingStarted() {
    // Entering ADVERTISING means there is no live connection, so a pairing overlay
    // that is still up belongs to a pairing attempt that ended without succeeding
    // (passkey never entered, SMP timeout, peer walked away) — issue #242. Nothing
    // else cleared it: onConnected() is the only other reset, so on a failed pairing
    // the passkey stayed on the panel indefinitely, hiding the active animation and
    // freezing shuffle, until the next successful connect or a manual
    // `anim indicator clear`.
    //
    // Conditional on purpose: a blind reset here would also stomp an unrelated
    // overlay (e.g. ExtensionsLoading during boot, which advertising start races).
    if (pattern_controller_get_current_indicator() == Indicator::BtPairing) {
        pattern_controller_reset_indicator();
    }

#if !defined(CONFIG_STATUS_LED)
    pattern_controller_request_indicator(Indicator::BtAdvertising);
#else
    status_led_set(1, StatusIndication::Breathing, StatusColor::Blue);
#endif
}

void PatternControllerBtObserver::onConnectingStarted() {
#if !defined(CONFIG_STATUS_LED)
    pattern_controller_request_indicator(Indicator::BtConnecting);
#else
    status_led_set(1, StatusIndication::Blinking, StatusColor::Blue);
#endif
}

void PatternControllerBtObserver::onConnected() {
    pattern_controller_reset_indicator();
#if defined(CONFIG_STATUS_LED)
    status_led_set(1, StatusIndication::Solid, StatusColor::Blue);
#endif
}

void PatternControllerBtObserver::onPairingCodeRequired(unsigned int pairingCode) {
    BtPairingAnimation::getInstance()->init();
    BtPairingAnimation::getInstance()->setPairingCode(pairingCode);
    pattern_controller_request_indicator(Indicator::BtPairing);
#if defined(CONFIG_STATUS_LED)
    status_led_set(1, StatusIndication::Blinking, StatusColor::Blue);
#endif
}

static PatternControllerBtObserver sPatternControllerBtObserver;

static int pattern_controller_register_bt_observer(void) {
    bluetooth_register_state_observer(&sPatternControllerBtObserver);
    return 0;
}
SYS_INIT(pattern_controller_register_bt_observer, APPLICATION, 0);
