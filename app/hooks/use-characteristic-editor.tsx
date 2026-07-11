import { CharacteristicBoolean } from "@/components/characteristic-boolean";
import { CharacteristicColor } from "@/components/characteristic-color";
import { CharacteristicDropdown } from "@/components/characteristic-dropdown";
import { CharacteristicFloat32 } from "@/components/characteristic-float32";
import { CharacteristicReadonly } from "@/components/characteristic-readonly";
import { CharacteristicUint32 } from "@/components/characteristic-uint32";
import { CharacteristicUtf8 } from "@/components/characteristic-utf8";
import {
    BLE_GATT_CPF_FORMAT_BOOLEAN, BLE_GATT_CPF_FORMAT_CUSTOM_COLOR, BLE_GATT_CPF_FORMAT_DROPDOWN_LIST,
    BLE_GATT_CPF_FORMAT_FLOAT32, BLE_GATT_CPF_FORMAT_UINT32, BLE_GATT_CPF_FORMAT_UTF8S,
    UUID_ANIMATION_NAME_CHARACTERISTIC, UUID_IS_ACTIVE_CHARACTERISTIC,
} from "@/constants/bluetooth";
import { CharacteristicInfo, useBluetooth } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { decodeFloat32FromBase64, decodeUint32FromBase64, decodeUtf8FromBase64, formatFloat32 } from "@/services/ble-value-codec";
import React, { useEffect, useRef, useState } from "react";
import { Animated } from "react-native";

/**
 * Shared characteristic read/write/decode/animate logic, lifted out of the old single-page
 * device-state screen so the per-service detail page ([serviceUuid].tsx) can reuse it unchanged.
 * Operates on the whole selectedDevice (not just one service) - the "only re-init on device
 * change" guard already works fine globally, and this keeps the extraction mechanical/low-risk.
 */
export function useCharacteristicEditor() {
    const { selectedDevice, writeToCharacteristic, writeServiceCharacteristic } = useBluetooth();
    const c = useThemeColors();

    // Local state for tracking pending input values (before BLE write)
    const [pendingValues, setPendingValues] = useState<Record<string, string>>({});
    // Ref mirror so notification effect can read pendingValues without it being a dep
    const pendingValuesRef = useRef<Record<string, string>>({});
    pendingValuesRef.current = pendingValues;
    // Last-seen encoded value per characteristic, so the notification-sync effect below only
    // reacts to characteristics whose stored value actually changed. Without this, the effect
    // re-runs on EVERY notification (e.g. the battery service's ~1 Hz VBUS telemetry) and
    // "pending differs from stored" is indistinguishable from "user is mid-edit" — clobbering
    // every in-progress text edit within a second.
    const lastSyncedValuesRef = useRef<Record<string, string>>({});
    // Track which device we've initialized for to avoid re-initializing on every update
    const [initializedDeviceId, setInitializedDeviceId] = useState<string | null>(null);
    // Track write status for each characteristic (success/error/notification/null)
    const [writeStatus, setWriteStatus] = useState<Record<string, 'success' | 'error' | 'notification' | null>>({});
    // Track animation values for color fade
    const fadeAnims = useRef<Record<string, Animated.Value>>({});

    // Initialize pendingValues only when device changes (not on every characteristic update)
    useEffect(() => {
        if (!selectedDevice) {
            setInitializedDeviceId(null);
            setPendingValues({});
            lastSyncedValuesRef.current = {};
            return;
        }

        // Only initialize if this is a new device
        if (selectedDevice.mac === initializedDeviceId) {
            return;
        }

        const initialValues: Record<string, string> = {};
        Object.entries(selectedDevice.characteristicsByService).forEach(([serviceUuid, chars]) => {
            Object.entries(chars).forEach(([charUuid, charInfo]) => {
                // Animation Name reuses the same characteristic UUID across every animation
                // service, which collides with this flat-by-charUuid map. It's surfaced via the
                // service header instead, so skip it here (see device-state render loop).
                if (charUuid === UUID_ANIMATION_NAME_CHARACTERISTIC) return;
                // Only initialize for text/numeric inputs (not boolean or color)
                if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_UTF8S && charInfo.value) {
                    try {
                        initialValues[charUuid] = decodeUtf8FromBase64(charInfo.value);
                    } catch (e) {
                        console.log(`Error decoding UTF8 value for ${charUuid}:`, e);
                    }
                } else if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_UINT32 && charInfo.value) {
                    try {
                        initialValues[charUuid] = String(decodeUint32FromBase64(charInfo.value));
                    } catch (e) {
                        console.log(`Error decoding UINT32 value for ${charUuid}:`, e);
                    }
                } else if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_FLOAT32 && charInfo.value) {
                    try {
                        initialValues[charUuid] = formatFloat32(decodeFloat32FromBase64(charInfo.value));
                    } catch (e) {
                        console.log(`Error decoding FLOAT32 value for ${charUuid}:`, e);
                    }
                }
            });
        });

        setPendingValues(initialValues);
        setInitializedDeviceId(selectedDevice.mac);
    }, [initializedDeviceId, selectedDevice, selectedDevice?.mac]);

    // Sync pendingValues when characteristic values are updated by BLE notifications.
    // The initialization effect above skips re-runs (by design) to avoid resetting user edits,
    // so this separate effect handles incoming notification updates.
    useEffect(() => {
        if (!selectedDevice) return;

        const prev = pendingValuesRef.current;
        const updates: Record<string, string> = {};

        Object.entries(selectedDevice.characteristics).forEach(([charUuid, charInfo]) => {
            if (!charInfo.value) return;
            // Only characteristics whose stored value actually changed since the last run may
            // touch pendingValues. Any other characteristic's notification re-runs this effect
            // too, and for those, a pending/stored mismatch just means the user is mid-edit.
            if (lastSyncedValuesRef.current[charUuid] === charInfo.value) return;
            lastSyncedValuesRef.current[charUuid] = charInfo.value;
            try {
                if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_UTF8S) {
                    const decoded = decodeUtf8FromBase64(charInfo.value);
                    if (prev[charUuid] !== decoded) updates[charUuid] = decoded;
                } else if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_UINT32) {
                    const decoded = String(decodeUint32FromBase64(charInfo.value));
                    if (prev[charUuid] !== decoded) updates[charUuid] = decoded;
                } else if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_FLOAT32) {
                    const decoded = formatFloat32(decodeFloat32FromBase64(charInfo.value));
                    if (prev[charUuid] !== decoded) updates[charUuid] = decoded;
                }
            } catch (e) { /* ignore decode errors */ }
        });

        if (Object.keys(updates).length > 0) {
            setPendingValues(prev => ({ ...prev, ...updates }));
            Object.keys(updates).forEach(charUuid => triggerStatusAnimation(charUuid, 'notification'));
        }
    }, [selectedDevice?.characteristics]); // fires only when a characteristic value reference changes

    // Helper to trigger write status animation
    function triggerStatusAnimation(charUuid: string, status: 'success' | 'error' | 'notification') {
        // Initialize fade animation if not exists
        if (!fadeAnims.current[charUuid]) {
            fadeAnims.current[charUuid] = new Animated.Value(1);
        }

        // Set status
        setWriteStatus(prev => ({ ...prev, [charUuid]: status }));

        // Reset animation value to 1 (full color)
        fadeAnims.current[charUuid].setValue(1);

        // Fade to 0 over 1 second
        Animated.timing(fadeAnims.current[charUuid], {
            toValue: 0,
            duration: 1000,
            useNativeDriver: false,
        }).start(() => {
            // Clear status after animation completes
            setWriteStatus(prev => ({ ...prev, [charUuid]: null }));
        });
    }

    function decodeValueForInput(cpfFormat: number | null, encodedValue: string, charUuid: string): string {
        try {
            if (cpfFormat === BLE_GATT_CPF_FORMAT_UINT32) {
                return String(decodeUint32FromBase64(encodedValue));
            }
            if (cpfFormat === BLE_GATT_CPF_FORMAT_FLOAT32) {
                return formatFloat32(decodeFloat32FromBase64(encodedValue));
            }
            if (cpfFormat === BLE_GATT_CPF_FORMAT_UTF8S) {
                return decodeUtf8FromBase64(encodedValue);
            }
            return decodeUtf8FromBase64(encodedValue);
        } catch (error) {
            console.log(`Error decoding value for ${charUuid}:`, error);
            return '';
        }
    }

    // Helper to write characteristic value to BLE with UI feedback. serviceUuid is required so
    // UUID_IS_ACTIVE_CHARACTERISTIC (reused across services) can be routed through the
    // service-aware write path instead of the ambiguous flat-map one.
    async function writeCharValue(serviceUuid: string, charUuid: string, newEncodedValue: string, previousEncodedValue: string) {
        const success = charUuid === UUID_IS_ACTIVE_CHARACTERISTIC
            ? await writeServiceCharacteristic(serviceUuid, charUuid, newEncodedValue)
            : await writeToCharacteristic(charUuid, newEncodedValue);

        const charInfo = charUuid === UUID_IS_ACTIVE_CHARACTERISTIC
            ? selectedDevice?.characteristicsByService?.[serviceUuid]?.[charUuid] ?? null
            : selectedDevice?.characteristics?.[charUuid] ?? null;

        if (success) {
            const decodedValue = decodeValueForInput(charInfo?.cpfFormat ?? null, newEncodedValue, charUuid);

            setPendingValues(prev => ({ ...prev, [charUuid]: decodedValue }));
            triggerStatusAnimation(charUuid, 'success');
        } else {
            const decodedPreviousValue = decodeValueForInput(charInfo?.cpfFormat ?? null, previousEncodedValue, charUuid);

            setPendingValues(prev => ({ ...prev, [charUuid]: decodedPreviousValue }));
            triggerStatusAnimation(charUuid, 'error');
        }
    }

    function renderCharacteristicInput(serviceUuid: string, charUuid: string, charInfo: CharacteristicInfo) {
        // Read-only telemetry (e.g. the Battery service's voltage/current) declares no write
        // property, so an editable input would only ever produce failing writes. Explicit
        // `=== false` checks so test fixtures / characteristics without these props keep the
        // old editable behavior.
        if (charInfo.characteristic.isWritableWithResponse === false &&
            charInfo.characteristic.isWritableWithoutResponse === false) {
            return <CharacteristicReadonly charInfo={charInfo} />;
        }
        if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_BOOLEAN) {
            return (
                <CharacteristicBoolean
                    charUuid={charUuid}
                    charInfo={charInfo}
                    onWrite={(uuid, encoded, previous) => writeCharValue(serviceUuid, uuid, encoded, previous)}
                />
            );
        }
        if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_UTF8S) {
            return (
                <CharacteristicUtf8
                    charUuid={charUuid}
                    charInfo={charInfo}
                    pendingValue={pendingValues[charUuid] ?? ''}
                    onChangeText={(uuid, text) => setPendingValues(prev => ({ ...prev, [uuid]: text }))}
                    onWrite={(uuid, encoded, previous) => writeCharValue(serviceUuid, uuid, encoded, previous)}
                />
            );
        }
        if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_UINT32) {
            return (
                <CharacteristicUint32
                    charUuid={charUuid}
                    charInfo={charInfo}
                    pendingValue={pendingValues[charUuid] ?? ''}
                    onChangeText={(uuid, text) => setPendingValues(prev => ({ ...prev, [uuid]: text }))}
                    onWrite={(uuid, encoded, previous) => writeCharValue(serviceUuid, uuid, encoded, previous)}
                />
            );
        }
        if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_FLOAT32) {
            return (
                <CharacteristicFloat32
                    charUuid={charUuid}
                    charInfo={charInfo}
                    pendingValue={pendingValues[charUuid] ?? ''}
                    onChangeText={(uuid, text) => setPendingValues(prev => ({ ...prev, [uuid]: text }))}
                    onWrite={(uuid, encoded, previous) => writeCharValue(serviceUuid, uuid, encoded, previous)}
                />
            );
        }
        if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_CUSTOM_COLOR) {
            return <CharacteristicColor charUuid={charUuid} charInfo={charInfo} />;
        }
        if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_DROPDOWN_LIST) {
            return <CharacteristicDropdown charUuid={charUuid} charInfo={charInfo} />;
        }
        return null;
    }

    function labelColorFor(charUuid: string): string | Animated.AnimatedInterpolation<string | number> {
        const status = writeStatus[charUuid];
        const fadeValue = fadeAnims.current[charUuid];
        const statusColor = status === 'success' ? c.success
            : status === 'notification' ? c.info
            : c.danger;
        return status && fadeValue
            ? fadeValue.interpolate({
                inputRange: [0, 1],
                outputRange: [c.textPrimary, statusColor],
            })
            : c.textPrimary;
    }

    return { renderCharacteristicInput, labelColorFor };
}
