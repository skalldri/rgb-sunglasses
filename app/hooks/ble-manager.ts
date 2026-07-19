import * as ExpoDevice from "expo-device";
import { PermissionsAndroid, Platform } from "react-native";
import { BleManager, BleManagerOptions, BleRestoredState } from "react-native-ble-plx";

// Shaped like ConnectingDevice (context/bluetooth-context.tsx): `mac` is
// ble-plx's device.id, which on iOS is an opaque per-phone CoreBluetooth
// peripheral UUID, not a real MAC address.
export type RestoredPeripheral = { mac: string; name: string };

// Module-scope, NOT React state: ble-plx registers restoreStateFunction inside
// the `new BleManager()` call below, at module-import time - before React
// mounts. Delivery, however, is ASYNCHRONOUS: the native willRestoreState
// result arrives as a bridge event on a later run-loop turn, so it can land
// before OR after the restoration adopter (hooks/use-ble-restoration.ts,
// issue #190) mounts. Hence the stash-or-deliver pair below rather than a
// read-once peek, which would silently lose the callback-after-mount ordering.
// iOS-only in practice (Android ignores restoreStateIdentifier).
//
// NOTE: iOS only relaunches the app for Core Bluetooth events after a SYSTEM
// termination (jetsam). After a user force-quit (App Switcher swipe) the
// restore callback never fires - platform limitation; the user must reopen
// the app. See the matching note in app/CLAUDE.md.
let restoredPeripheral: RestoredPeripheral | null = null;
let restoredSubscriber: ((peripheral: RestoredPeripheral) => void) | null = null;

// Exported (rather than inlined into bleManagerOptions) so unit tests can
// drive it directly - the jest BleManager mock ignores constructor options.
export function handleRestoredState(bleRestoredState: BleRestoredState | null): void {
    if (bleRestoredState == null) {
        console.log(`No restored state for BleManager`);
        // BleManager was constructed for the first time.
        return;
    }
    // Defensive: this runs in the native->JS restore callback, outside any
    // error boundary - a missing peripherals array must degrade to a no-op,
    // never throw at startup.
    const peripherals = bleRestoredState.connectedPeripherals ?? [];
    console.log(
        `BleManager state restored with ${peripherals.length} peripheral(s): ` +
        peripherals.map(d => `${d.localName || d.name || '?'} (${d.id})`).join(', ')
    );
    if (peripherals.length === 0) return;
    if (peripherals.length > 1) {
        // The app only ever manages one device; adopt the first and say so.
        console.warn(`Restored ${peripherals.length} peripherals; adopting only the first`);
    }
    const device = peripherals[0];
    // || not ??: a restored peripheral can carry an EMPTY-string name, which
    // should fall through to the default just like null would.
    const peripheral: RestoredPeripheral = {
        mac: device.id,
        name: device.localName || device.name || 'RGB Sunglasses',
    };
    if (restoredSubscriber) {
        restoredSubscriber(peripheral);
    } else {
        restoredPeripheral = peripheral;
    }
}

// Deliver-once handoff to the restoration adopter: if the restore callback
// already fired, the subscriber is called immediately (and the stash cleared);
// otherwise it's called when/if the callback lands. Either way the peripheral
// is delivered to exactly ONE subscriber exactly once - a remounted (or
// duplicate) adopter subscribing later gets nothing, which is what makes a
// double mount unable to start two competing reconnect loops for the same
// restored link. Returns an unsubscribe fn (a later subscriber may then
// receive a subsequently-stashed peripheral).
export function subscribeRestoredPeripheral(
    subscriber: (peripheral: RestoredPeripheral) => void
): () => void {
    if (restoredPeripheral) {
        const peripheral = restoredPeripheral;
        restoredPeripheral = null;
        subscriber(peripheral);
        return () => {};
    }
    restoredSubscriber = subscriber;
    return () => {
        if (restoredSubscriber === subscriber) {
            restoredSubscriber = null;
        }
    };
}

// Test-only reset of the stash (delivery makes consumption implicit in app code).
export function consumeRestoredPeripheral(): void {
    restoredPeripheral = null;
}

const bleManagerOptions: BleManagerOptions = {
    restoreStateIdentifier: 'bleManagerRestoredState',
    restoreStateFunction: handleRestoredState,
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
