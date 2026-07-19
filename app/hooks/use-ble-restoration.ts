import { RestoredPeripheral, subscribeRestoredPeripheral } from "@/hooks/ble-manager";
import { useBleConnection } from "@/hooks/use-ble-connection";
import { useEffect, useRef, useState } from "react";

// iOS Core Bluetooth state-restoration adopter (issue #190): when iOS jetsams
// the app and later relaunches it (in the background for a BLE event, or by
// the user opening it) while Core Bluetooth preserved a managed peripheral,
// the restore callback in hooks/ble-manager.ts delivers that peripheral here
// and this hook drives the issue-#124 auto-reconnect loop. The loop's iOS
// pending connect resolves immediately on the still-connected peripheral and
// then runs the normal discovery/monitor/selection path. Headless-safe: a
// background relaunch runs this with no user present; the loop pins the
// "Reconnecting…" row as the cancel affordance if the user opens the app
// mid-adoption.
//
// The handoff is subscription-based, NOT a read-once peek at mount: ble-plx
// delivers the restore state as an async bridge event that can land before or
// after this hook mounts (see ble-manager.ts). Delivery is once-only, so a
// duplicate/remounted adopter can't start a second competing loop; the
// startedRef guards the same within one instance (e.g. a double-fired effect),
// since a second startReconnectLoop() would bump the generation and make the
// first attempt tear down the very link restoration preserved.
//
// A peripheral the user intentionally disconnected from can never show up here:
// disconnect() calls cancelDeviceConnection(), after which iOS stops managing
// the peripheral for this app - so losing the in-memory intentionalDisconnectRef
// across the relaunch is fine.
//
// Mount once at the root, inside BluetoothProvider.
export function useBleRestorationAdopt(): void {
    const [restored, setRestored] = useState<RestoredPeripheral | null>(null);

    // '' until (unless) a peripheral is delivered - same convention as
    // useBleAppStateVerify; the starter effect below never runs the loop then.
    const { startReconnectLoop } = useBleConnection(restored?.mac ?? '', restored?.name ?? '');

    // subscribeRestoredPeripheral returns its unsubscribe fn, which doubles as
    // this effect's cleanup.
    useEffect(() => subscribeRestoredPeripheral(setRestored), []);

    const startedRef = useRef(false);
    useEffect(() => {
        if (!restored || startedRef.current) return;
        startedRef.current = true;
        console.log(`State restoration: re-adopting ${restored.name} (${restored.mac})`);
        // This render's startReconnectLoop closure is bound to the restored
        // mac/name (the effect runs after the hook re-bound to them above).
        void startReconnectLoop();
        // startReconnectLoop's identity churns per render; the one-shot is
        // keyed on the delivered peripheral only.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [restored]);
}
