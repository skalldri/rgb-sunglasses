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

// Poll interval for targets the connected firmware does not declare notifiable.
// Matches the 2 s tick the battery detail page used before it became notify-driven,
// so behaviour against older firmware is unchanged rather than merely non-frozen.
const POLL_FALLBACK_MS = 2000;
// One retry when the device is not yet in context at focus time.
const ARM_RETRY_MS = 1500;

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
        const subscriptions: { remove: () => void }[] = [];
        let pollTimer: ReturnType<typeof setInterval> | null = null;
        let armTimer: ReturnType<typeof setTimeout> | null = null;

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

        // Fire-and-forget read that is safe to call from a deferred context: read()
        // can throw SYNCHRONOUSLY once the link is gone, which a bare .catch() misses
        // (app/CLAUDE.md). Used by the seed read, the poll fallback and the
        // dropdown re-read alike, so none of them can regress that rule
        // independently.
        const safeRead = (characteristic: { read?: () => Promise<{ value: string | null }> },
                          label: string, onValue: (value: string) => void) => {
            try {
                const pending = characteristic.read?.();
                if (!pending) return;
                pending
                    .then(read => { if (read.value) onValue(read.value); })
                    .catch(err => console.log(`Scoped read failed for ${label}:`, err));
            } catch (err) {
                console.log(`Scoped read could not start for ${label}:`, err);
            }
        };

        // Characteristics that need polling because the connected firmware does not
        // declare them notifiable. See POLL_FALLBACK_MS below for why this exists.
        const pollTargets: { serviceUuid: string; charUuid: string;
                             characteristic: { read?: () => Promise<{ value: string | null }> } }[] = [];

        const arm = () => {
            if (generationRef.current !== generation) return;
            const device = deviceRef.current;

            for (const { serviceUuid, charUuid } of targetsRef.current) {
                const info = device?.characteristicsByService?.[serviceUuid]?.[charUuid];
                if (!info) continue;

                if (!info.characteristic.isNotifiable) {
                    // Older firmware declares these read-only (see the notify-contract
                    // test in fw/tests/bluetooth/battery_service). Subscribing is
                    // impossible, so fall back to polling rather than silently
                    // rendering the connect-time snapshot forever — the app and the
                    // firmware ship independently, and the app is expected to be
                    // updated FIRST, so new-app-on-old-firmware is a normal state.
                    pollTargets.push({ serviceUuid, charUuid, characteristic: info.characteristic });
                    continue;
                }

                // Seeded by an explicit read: a screen regaining focus (or a plain
                // reconnect) triggers no notification on its own, so subscribe-only
                // would render stale values until the device happened to change.
                // Precedent + rationale: services/mcuboot-updater-client.ts.
                //
                // Subscribe BEFORE reading, and let any notification that lands first
                // win: a notification arriving while the seed read is in flight is
                // strictly fresher than that read, and applying the read afterwards
                // would snap the value backwards (app/CLAUDE.md, "A deferred read must
                // compare-and-swap before it applies"). The reverse order — read fully
                // before subscribing — would instead miss a change occurring in that
                // window, so neither ordering alone is sufficient; this flag is what
                // makes the pair safe.
                let notified = false;

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
                        notified = true;

                        if (info.cpfFormat === BLE_GATT_CPF_FORMAT_DROPDOWN_LIST) {
                            // A dropdown-list characteristic notifies only its first token
                            // (the new selection), not the canonical "selected\nothers..."
                            // list: bt_gatt_notify can't fragment across ATT PDUs, so the
                            // firmware deliberately sends a short preview. Trusting these
                            // bytes would collapse the picker to a single option — re-read
                            // for the full value. See fw/CLAUDE.md (BtGattNotifyTraits).
                            safeRead(characteristic, charUuid,
                                     value => applyValue(serviceUuid, charUuid, value));
                            return;
                        }

                        applyValue(serviceUuid, charUuid, characteristic.value);
                    });
                    subscriptions.push(subscription);
                } catch (err) {
                    console.log(`Scoped monitor could not start for ${charUuid}:`, err);
                }

                safeRead(info.characteristic, charUuid, value => {
                    if (notified) return;  // a live notification already superseded this read
                    applyValue(serviceUuid, charUuid, value);
                });
            }

            if (pollTargets.length > 0) {
                const poll = () => pollTargets.forEach(t =>
                    safeRead(t.characteristic, t.charUuid,
                             value => applyValue(t.serviceUuid, t.charUuid, value)));
                pollTimer = setInterval(poll, POLL_FALLBACK_MS);
            }
        };

        // The device can arrive after focus — a screen mounted during connect, or a
        // reconnect completing while it stays focused. The effect deliberately takes
        // no context deps (that is the read-loop hazard), so without this retry it
        // would subscribe to nothing and never try again until the user navigated
        // away and back. One deferred re-arm covers the realistic window; the
        // generation guard makes a late fire from a superseded pass a no-op.
        if (deviceRef.current?.characteristicsByService) {
            arm();
        } else {
            armTimer = setTimeout(arm, ARM_RETRY_MS);
        }

        return () => {
            // Bumping the generation before removing means any callback still in
            // flight from this pass is ignored rather than writing a stale value.
            generationRef.current++;
            if (armTimer) clearTimeout(armTimer);
            if (pollTimer) clearInterval(pollTimer);
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
