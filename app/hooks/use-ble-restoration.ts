import { consumeRestoredPeripheral, peekRestoredPeripheral } from "@/hooks/ble-manager";
import { useBleConnection } from "@/hooks/use-ble-connection";
import { useEffect, useState } from "react";

// iOS Core Bluetooth state-restoration adopter (issue #190): when iOS jetsams
// the app and later relaunches it in the background for a BLE event on a
// peripheral it was managing, the restore callback in hooks/ble-manager.ts has
// stashed that peripheral before React mounted. This hook consumes the stash
// once and drives the issue-#124 auto-reconnect loop, whose iOS pending connect
// resolves immediately on the still-connected peripheral and then runs the
// normal discovery/monitor/selection path. Headless-safe: a background relaunch
// runs this with no user present; the loop pins the "Reconnecting…" row as the
// cancel affordance if the user does open the app mid-adoption.
//
// A peripheral the user intentionally disconnected from can never show up here:
// disconnect() calls cancelDeviceConnection(), after which iOS stops managing
// the peripheral for this app - so losing the in-memory intentionalDisconnectRef
// across the relaunch is fine.
//
// Mount once at the root, inside BluetoothProvider.
export function useBleRestorationAdopt(): void {
    // Peek in a useState initializer: a pure read, and captured once so the
    // useBleConnection args stay stable for this mount (StrictMode-safe).
    const [restored] = useState(() => peekRestoredPeripheral());

    // '' when nothing was restored - same convention as useBleAppStateVerify;
    // the effect below never starts the loop in that case.
    const { startReconnectLoop } = useBleConnection(restored?.mac ?? '', restored?.name ?? '');

    useEffect(() => {
        if (!restored) return;
        // Consume before starting: a later remount peeks null and no-ops. A
        // double-fired effect is additionally guarded by the context-level
        // connectPromises dedup and the loop's already-selected break.
        consumeRestoredPeripheral();
        console.log(`State restoration: re-adopting ${restored.name} (${restored.mac})`);
        void startReconnectLoop();
        // Mount-only by design; startReconnectLoop's identity churns per render.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);
}
