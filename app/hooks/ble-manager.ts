import * as ExpoDevice from "expo-device";
import { PermissionsAndroid, Platform } from "react-native";
import { BleManager, BleManagerOptions, BleRestoredState } from "react-native-ble-plx";

const bleManagerOptions: BleManagerOptions = {
    restoreStateIdentifier: 'bleManagerRestoredState',
    restoreStateFunction: (bleRestoredState: BleRestoredState | null) => {
        if (bleRestoredState == null) {
            console.log(`No restored state for BleManager`);
            // BleManager was constructed for the first time.
        } else {
            console.log(`State was restored! ${JSON.stringify(bleRestoredState)}`);
            // BleManager was restored. Check `bleRestoredState.connectedPeripherals` property.
        }
    },
};

// Re-running this module constructs a fresh BleManager, whose native side
// registers a brand-new BLE client. If the previous module instance's client
// (plus any scanner registration it held) isn't destroyed first, it stays alive
// in the app process; after a few of these Android starts refusing new scanner
// clients for this app with SCAN_FAILED_APPLICATION_REGISTRATION_FAILED
// (surfaced as ble-plx "Cannot start scanning operation ... (code 6)"), and only
// restarting the phone's Bluetooth stack clears it (hardware-observed, issue #90
// follow-up).
//
// This hook only covers the SAME-JS-RUNTIME reload: Fast Refresh (and other
// in-place module re-evaluations) keep the Hermes runtime alive, so globalThis -
// and the destroy hook we stash on it below - survive into the re-run, letting
// the new module instance tear down its predecessor's native client before
// creating its own. It does NOT cover a full reload (RN dev-menu "Reload",
// execbro reload_app, a fresh app start): those spin up a NEW Hermes runtime, so
// globalThis is empty and __blePlxDestroyPreviousClient reads undefined - there's
// no in-process handle to the old client to call destroy() on from here. A full
// reload that leaves the previous native client alive still needs the Bluetooth-
// stack reset above; recovering that case automatically would require native-side
// teardown on runtime disposal, which this JS-only hook can't reach.
declare global {
    // eslint-disable-next-line no-var
    var __blePlxDestroyPreviousClient: (() => void) | undefined;
}
if (globalThis.__blePlxDestroyPreviousClient) {
    try {
        globalThis.__blePlxDestroyPreviousClient();
        console.log('Destroyed previous JS context\'s BleManager native client');
    } catch (error) {
        console.log('Failed to destroy previous BleManager native client:', error);
    }
}

export const bleManager = new BleManager(bleManagerOptions);

globalThis.__blePlxDestroyPreviousClient = () => {
    // destroy() is async and returns a Promise; the try/catch at the call site
    // above only catches a synchronous throw, so route the rejection through a
    // .catch() here - otherwise a failed teardown surfaces as an unhandled
    // promise rejection (a red LogBox in dev) in the new context.
    Promise.resolve(bleManager.destroy()).catch(error =>
        console.log('Error while destroying previous BleManager native client:', error)
    );
};

const requestAndroid31Permissions = async () => {
    const bluetoothScanPermission = await PermissionsAndroid.request(
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
        {
            title: "Bluetooth Permission",
            message: "Required to discover nearby Bluetooth devices",
            buttonPositive: "OK",
        }
    );
    const bluetoothConnectPermission = await PermissionsAndroid.request(
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
        {
            title: "Bluetooth Permission",
            message: "Required to connect to Bluetooth devices",
            buttonPositive: "OK",
        }
    );
    // No location permission on API 31+: BLUETOOTH_SCAN carries
    // android:usesPermissionFlags="neverForLocation" (set by the ble-plx Expo
    // plugin via "neverForLocation": true in app.json), which asserts scan
    // results are never used to derive location — so ACCESS_FINE_LOCATION is
    // not required for scanning and isn't even declared beyond maxSdkVersion
    // 30 in the manifest. Requesting it here would always come back denied.
    // Trade-off accepted with the flag: Android filters some location-beacon
    // advertisement frames from scans; the board is a plain connectable
    // advertiser and is unaffected (issue #189, B-PR1).

    return (
        bluetoothScanPermission === "granted" &&
        bluetoothConnectPermission === "granted"
    );
};

export const requestPermissions = async (): Promise<boolean> => {
    if (Platform.OS === "android") {
        if ((ExpoDevice.platformApiLevel ?? -1) < 31) {
            const granted = await PermissionsAndroid.request(
                PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
                {
                    title: "Location Permission",
                    message: "Required for Bluetooth device scanning on Android",
                    buttonPositive: "OK",
                }
            );
            return granted === PermissionsAndroid.RESULTS.GRANTED;
        } else {
            return await requestAndroid31Permissions();
        }
    } else {
        return true;
    }
};
