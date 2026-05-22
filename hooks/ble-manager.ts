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

export const bleManager = new BleManager(bleManagerOptions);

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
