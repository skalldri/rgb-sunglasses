#pragma once

/**
 * @brief Observer for Bluetooth state transitions.
 *
 * Implement this interface to be notified of BT advertising, connecting,
 * connected, and pairing events without depending on the BT stack.
 * Register an instance with bluetooth_register_state_observer().
 */
class BtStateObserver
{
public:
    virtual ~BtStateObserver() = default;

    virtual void onAdvertisingStarted() = 0;
    virtual void onConnectingStarted() = 0;
    virtual void onConnected() = 0;

    /**
     * @param pairingCode The passkey the user must enter on the remote device.
     */
    virtual void onPairingCodeRequired(unsigned int pairingCode) = 0;
};

/**
 * @brief Register a BT state observer.
 *
 * Must be called before the BT thread starts (e.g. from a SYS_INIT hook)
 * to avoid missing the initial ADVERTISING transition.
 * Only one observer is supported at a time.
 */
void bluetooth_register_state_observer(BtStateObserver *observer);
