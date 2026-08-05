import {
    BLE_GATT_CPF_FORMAT_DROPDOWN_LIST,
    UUID_IS_ACTIVE_CHARACTERISTIC,
    UUID_SHUFFLE_INCLUDE_CHARACTERISTIC,
} from "@/constants/bluetooth";
import { useBluetooth } from "@/context/bluetooth-context";
import { useFocusEffect } from "expo-router";
import React from "react";

/** A characteristic to keep subscribed while the owning screen is focused. */
export interface ScopedMonitorTarget {
    serviceUuid: string;
    charUuid: string;
}

/**
 * Subscribes to BLE notifications only while the owning screen is focused, and
 * unsubscribes on blur.
 *
 * WHY THIS EXISTS: Android caps concurrent GATT notification registrations at ~15
 * per app (BTA_GATTC_NOTIF_REG_MAX) and silently drops the overflow — the CCC write
 * still succeeds, so the firmware notifies into a void. That broke every firmware
 * update in fw-v2.1.0 (/debug-ble section 4a). The cap is on *concurrent
 * registrations*, not on how many characteristics are declared notifiable, so the
 * firmware declares notify wherever a real device-side push exists and the app keeps
 * only a tiny always-on set plus whatever the focused screen needs. See the rule
 * block in fw/src/core_config.cpp.
 *
 * Unsubscribing genuinely frees a slot: ble-plx's `remove()` is a native
 * `cancelTransaction`, which reaches rxandroidble's teardown and
 * `BluetoothGatt.setCharacteristicNotification(char, false)` — the Android-side
 * deregister — not merely a JS listener detach.
 *
 * Values land in the normal context sinks, so consumers read them exactly as they
 * read notified values today: `updateServiceCharacteristicValue` for the two UUIDs
 * that are reused across every animation service, `updateCharValue` otherwise.
 */
export function useScopedCharacteristicMonitors(targets: ScopedMonitorTarget[]) {
    const { selectedDevice, updateCharValue, updateServiceCharacteristicValue } = useBluetooth();

    // Everything the effect touches goes through a ref so the effect itself can
    // declare EMPTY deps. Depending on context-derived values instead is a feedback
    // loop — the update lands in context, context hands back fresh objects, the
    // callback identity changes, the focus effect re-runs, forever. That shipped
    // once already at ~11 reads/second (app/CLAUDE.md, "What the device-free loop
    // CANNOT catch"). `targets` is included here deliberately: callers build it
    // inline, so it is a new array every render and must never be a dependency.
    const targetsRef = React.useRef(targets);
    targetsRef.current = targets;
    const deviceRef = React.useRef(selectedDevice);
    deviceRef.current = selectedDevice;
    const updateCharValueRef = React.useRef(updateCharValue);
    updateCharValueRef.current = updateCharValue;
    const updateServiceValueRef = React.useRef(updateServiceCharacteristicValue);
    updateServiceValueRef.current = updateServiceCharacteristicValue;

    // rxandroidble tears a notification down fire-and-forget and does NOT serialize
    // it against a following re-subscribe, so a fast blur -> focus on the same
    // characteristic races. Every subscription pass captures this counter and drops
    // its own late callbacks once superseded.
    const generationRef = React.useRef(0);

    useFocusEffect(React.useCallback(() => {
        const generation = ++generationRef.current;
        const device = deviceRef.current;
        const subscriptions: { remove: () => void }[] = [];

        const applyValue = (serviceUuid: string, charUuid: string, value: string) => {
            if (generationRef.current !== generation) return;  // superseded pass
            if (charUuid === UUID_IS_ACTIVE_CHARACTERISTIC ||
                charUuid === UUID_SHUFFLE_INCLUDE_CHARACTERISTIC) {
                // Reused identically across every animation service, so these are
                // absent from the flat map and must be addressed service-aware.
                updateServiceValueRef.current(serviceUuid, charUuid, value);
            } else {
                updateCharValueRef.current(charUuid, value);
            }
        };

        for (const { serviceUuid, charUuid } of targetsRef.current) {
            const info = device?.characteristicsByService?.[serviceUuid]?.[charUuid];
            if (!info || !info.characteristic.isNotifiable) continue;

            // Read first, THEN monitor. A screen that regains focus (or a plain
            // reconnect) triggers no notification on its own, so subscribe-only
            // would render stale values until the device happened to change.
            // Precedent + rationale: services/mcuboot-updater-client.ts.
            try {
                info.characteristic.read?.()
                    ?.then(read => {
                        if (read.value) applyValue(serviceUuid, charUuid, read.value);
                    })
                    .catch(err => console.log(`Scoped seed read failed for ${charUuid}:`, err));
            } catch (err) {
                // read() can throw synchronously once the link is gone; a bare
                // .catch() would miss that (app/CLAUDE.md).
                console.log(`Scoped seed read could not start for ${charUuid}:`, err);
            }

            try {
                const subscription = info.characteristic.monitor((error, characteristic) => {
                    if (error) {
                        const errorStr = error?.message || String(error);
                        // remove() delivers OperationCancelled to this callback, and a
                        // dropped link delivers a disconnect error — both are the
                        // normal way a subscription ends, not failures worth surfacing.
                        if (errorStr.includes('cancel') || errorStr.includes('Cancel') ||
                            errorStr.includes('disconnect') || errorStr.includes('Disconnect')) {
                            return;
                        }
                        console.error(`Scoped notification error for ${charUuid}:`, error);
                        return;
                    }
                    if (!characteristic?.value) return;

                    if (info.cpfFormat === BLE_GATT_CPF_FORMAT_DROPDOWN_LIST) {
                        // A dropdown-list characteristic notifies only its first token
                        // (the new selection), not the canonical "selected\nothers..."
                        // list: bt_gatt_notify can't fragment across ATT PDUs, so the
                        // firmware deliberately sends a short preview. Trusting these
                        // bytes would collapse the picker to a single option — re-read
                        // for the full value. See fw/CLAUDE.md (BtGattNotifyTraits).
                        characteristic.read()
                            .then(read => {
                                if (read.value) applyValue(serviceUuid, charUuid, read.value);
                            })
                            .catch(err => console.log(`Failed to re-read ${charUuid} after notification:`, err));
                        return;
                    }

                    applyValue(serviceUuid, charUuid, characteristic.value);
                });
                subscriptions.push(subscription);
            } catch (err) {
                console.log(`Scoped monitor could not start for ${charUuid}:`, err);
            }
        }

        return () => {
            // Bumping the generation before removing means any callback still in
            // flight from this pass is ignored rather than writing a stale value.
            generationRef.current++;
            subscriptions.forEach(sub => {
                try {
                    sub.remove();
                } catch (err) {
                    console.log('Scoped monitor removal failed:', err);
                }
            });
        };
    }, []));
}
