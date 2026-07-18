import { useBluetooth } from "@/context/bluetooth-context";
import { useBleConnection } from "@/hooks/use-ble-connection";
import { useEffect, useRef } from "react";
import { AppState } from "react-native";

// Foreground connection verifier (issue #124): when the app returns to the
// foreground, check that the OS-level BLE link still backs the app's "connected"
// state, and recover (cleanup + auto-reconnect) if not. This is primarily the
// iOS safety net - a suspended app can miss the disconnect event entirely, so
// nothing else would ever fire the reconnect path - and costs one native call
// per foregrounding on Android. Mount once at the root, inside BluetoothProvider.
export function useBleAppStateVerify(): void {
    const { selectedDevice } = useBluetooth();

    // When nothing is selected the mac is '' and verifyConnection() no-ops.
    const { verifyConnection } = useBleConnection(selectedDevice?.mac ?? '', selectedDevice?.name ?? '');

    // The listener is registered once; route calls through a ref so it always
    // invokes the latest verifyConnection closure (bound to the currently
    // selected device), not the one from mount time.
    const verifyRef = useRef(verifyConnection);
    verifyRef.current = verifyConnection;

    useEffect(() => {
        const subscription = AppState.addEventListener('change', (state) => {
            if (state === 'active') {
                void verifyRef.current();
            }
        });
        return () => subscription.remove();
    }, []);
}
