#pragma once

#include <bluetooth/bt_state_observer.h>

/**
 * @brief The BtStateObserver that drives the panel indicator overlay and the
 *        onboard status LED from Bluetooth state transitions.
 *
 * Lives in its own translation unit (rather than inside pattern_controller.cpp)
 * so it can be unit-tested without linking the render thread, LED controller,
 * filesystem and extension host that pattern_controller.cpp pulls in.
 *
 * A file-scope instance registers itself with bluetooth_register_state_observer()
 * from a SYS_INIT hook; nothing else needs to reference this class.
 */
class PatternControllerBtObserver : public BtStateObserver {
   public:
    void onAdvertisingStarted() override;
    void onConnectingStarted() override;
    void onConnected() override;
    void onPairingCodeRequired(unsigned int pairingCode) override;
};
