#include <zephyr/ztest.h>
#include <bluetooth/bt_state_observer.h>

// ---------------------------------------------------------------------------
// Minimal recording observer
// ---------------------------------------------------------------------------

struct RecordingObserver : public BtStateObserver
{
    int advertisingCount = 0;
    int connectingCount = 0;
    int connectedCount = 0;
    int pairingCount = 0;
    unsigned int lastPairingCode = 0;

    void onAdvertisingStarted() override { advertisingCount++; }
    void onConnectingStarted() override  { connectingCount++; }
    void onConnected() override          { connectedCount++; }
    void onPairingCodeRequired(unsigned int code) override
    {
        pairingCount++;
        lastPairingCode = code;
    }

    void reset()
    {
        advertisingCount = 0;
        connectingCount  = 0;
        connectedCount   = 0;
        pairingCount     = 0;
        lastPairingCode  = 0;
    }
};

static RecordingObserver sObserver;

static void reset_observer(void *f)
{
    sObserver.reset();
}

ZTEST_SUITE(bt_state_observer_tests, NULL, NULL, reset_observer, reset_observer, NULL);

// ---------------------------------------------------------------------------
// Tests — exercise the interface contract directly.
// These tests verify the observer receives exactly the right notifications
// when the state machine calls its methods.
// ---------------------------------------------------------------------------

ZTEST(bt_state_observer_tests, test_observer_receives_advertising)
{
    BtStateObserver &obs = sObserver;
    obs.onAdvertisingStarted();

    zassert_equal(sObserver.advertisingCount, 1, "Expected advertising notification");
    zassert_equal(sObserver.connectingCount, 0, "No other calls expected");
    zassert_equal(sObserver.connectedCount, 0, "No other calls expected");
    zassert_equal(sObserver.pairingCount, 0, "No other calls expected");
}

ZTEST(bt_state_observer_tests, test_observer_receives_connecting)
{
    BtStateObserver &obs = sObserver;
    obs.onConnectingStarted();

    zassert_equal(sObserver.connectingCount, 1, "Expected connecting notification");
    zassert_equal(sObserver.advertisingCount, 0, "No other calls expected");
}

ZTEST(bt_state_observer_tests, test_observer_receives_connected)
{
    BtStateObserver &obs = sObserver;
    obs.onConnected();

    zassert_equal(sObserver.connectedCount, 1, "Expected connected notification");
}

ZTEST(bt_state_observer_tests, test_observer_receives_pairing_code)
{
    BtStateObserver &obs = sObserver;
    obs.onPairingCodeRequired(123456);

    zassert_equal(sObserver.pairingCount, 1, "Expected pairing notification");
    zassert_equal(sObserver.lastPairingCode, 123456U, "Expected correct pairing code");
}

ZTEST(bt_state_observer_tests, test_observer_receives_zero_pairing_code)
{
    BtStateObserver &obs = sObserver;
    obs.onPairingCodeRequired(0);

    zassert_equal(sObserver.pairingCount, 1, "Expected pairing notification for code 0");
    zassert_equal(sObserver.lastPairingCode, 0U, "Expected pairing code 0 to be forwarded");
}

ZTEST(bt_state_observer_tests, test_multiple_events_accumulated)
{
    BtStateObserver &obs = sObserver;
    obs.onAdvertisingStarted();
    obs.onAdvertisingStarted();
    obs.onConnectingStarted();

    zassert_equal(sObserver.advertisingCount, 2, "Expected two advertising notifications");
    zassert_equal(sObserver.connectingCount, 1, "Expected one connecting notification");
}

