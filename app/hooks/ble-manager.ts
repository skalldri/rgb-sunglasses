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

// A JS reload (Metro Fast Refresh full reload, dev-menu Reload, execbro
// reload_app) tears down this module's JS context but NOT the app process -
// each reload re-runs this module and constructs a fresh BleManager, whose
// native side registers a brand-new BLE client while the previous context's
// client (plus any scanner registration it held) stays alive in the process
// forever. After a few reloads Android starts refusing new scanner clients for
// this app with SCAN_FAILED_APPLICATION_REGISTRATION_FAILED (surfaced as
// ble-plx "Cannot start scanning operation ... (code 6)"), and only restarting
// the phone's Bluetooth stack clears it (hardware-observed, issue #90
// follow-up). Stash each context's destroy hook on globalThis so the next
// context can release its predecessor's native client before creating its own.
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

globalThis.__blePlxDestroyPreviousClient = () => bleManager.destroy();

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
    const fineLocationPermission = await PermissionsAndroid.request(
        PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
        {
            title: "Location Permission",
            message: "Required for Bluetooth device scanning on Android",
            buttonPositive: "OK",
        }
    );

    return (
        bluetoothScanPermission === "granted" &&
        bluetoothConnectPermission === "granted" &&
        fineLocationPermission === "granted"
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
