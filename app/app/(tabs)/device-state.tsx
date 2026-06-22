import { CharacteristicBoolean } from "@/components/characteristic-boolean";
import { CharacteristicColor } from "@/components/characteristic-color";
import { CharacteristicDropdown } from "@/components/characteristic-dropdown";
import { CharacteristicFloat32 } from "@/components/characteristic-float32";
import { CharacteristicUint32 } from "@/components/characteristic-uint32";
import { CharacteristicUtf8 } from "@/components/characteristic-utf8";
import { ThemedText } from "@/components/themed-text";
import { AppButton } from "@/components/ui/app-button";
import { Badge } from "@/components/ui/badge";
import { Card } from "@/components/ui/card";
import { Divider } from "@/components/ui/divider";
import { EmptyState } from "@/components/ui/empty-state";
import { ListRow } from "@/components/ui/list-row";
import { Section } from "@/components/ui/section";
import { BLE_GATT_CPF_FORMAT_BOOLEAN, BLE_GATT_CPF_FORMAT_CUSTOM_COLOR, BLE_GATT_CPF_FORMAT_DROPDOWN_LIST, BLE_GATT_CPF_FORMAT_FLOAT32, BLE_GATT_CPF_FORMAT_UINT32, BLE_GATT_CPF_FORMAT_UTF8S, getCharacteristicName, getServiceName, UUID_ANIMATION_NAME_CHARACTERISTIC, UUID_GENERIC_ACCESS_SERVICE, UUID_GENERIC_ATTRIBUTE_SERVICE } from "@/constants/bluetooth";
import { Spacing } from "@/constants/theme";
import { CharacteristicInfo, useBluetooth } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { decodeFloat32FromBase64, decodeUint32FromBase64, decodeUtf8FromBase64, formatFloat32 } from "@/services/ble-value-codec";
import { SMP_CHARACTERISTIC_UUID, SMP_SERVICE_UUID } from "@/services/mcumgr";
import { Link } from "expo-router";
import React, { useEffect, useRef, useState } from "react";
import { Animated, KeyboardAvoidingView, Platform, ScrollView, StyleSheet, View } from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";


export default function DeviceStateScreen() {
    const { selectedDevice, writeToCharacteristic } = useBluetooth();
    const c = useThemeColors();

    // Local state for tracking pending input values (before BLE write)
    const [pendingValues, setPendingValues] = useState<Record<string, string>>({});
    // Ref mirror so notification effect can read pendingValues without it being a dep
    const pendingValuesRef = useRef<Record<string, string>>({});
    pendingValuesRef.current = pendingValues;
    // Track which device we've initialized for to avoid re-initializing on every update
    const [initializedDeviceId, setInitializedDeviceId] = useState<string | null>(null);
    // Track write status for each characteristic (success/error/notification/null)
    const [writeStatus, setWriteStatus] = useState<Record<string, 'success' | 'error' | 'notification' | null>>({});
    // Track animation values for color fade
    const fadeAnims = useRef<Record<string, Animated.Value>>({});

    if (selectedDevice != null) {
        console.log(`Connected to device: ${selectedDevice.name} `);
    }

    // Initialize pendingValues only when device changes (not on every characteristic update)
    useEffect(() => {
        if (!selectedDevice) {
            setInitializedDeviceId(null);
            setPendingValues({});
            return;
        }

        // Only initialize if this is a new device
        if (selectedDevice.mac === initializedDeviceId) {
            console.log(`Device ${selectedDevice.mac} already initialized, skipping`);
            return;
        }

        const initialValues: Record<string, string> = {};
        Object.entries(selectedDevice.characteristicsByService).forEach(([serviceUuid, chars]) => {
            Object.entries(chars).forEach(([charUuid, charInfo]) => {
                // Animation Name reuses the same characteristic UUID across every animation
                // service, which collides with this flat-by-charUuid map. It's surfaced via the
                // service header instead, so skip it here (see device-state.tsx render loop).
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

    // Helper to write characteristic value to BLE with UI feedback
    async function writeCharValue(charUuid: string, newEncodedValue: string, previousEncodedValue: string) {
        const success = await writeToCharacteristic(charUuid, newEncodedValue);

        // Get the characteristic info using the flat lookup
        const charInfo = selectedDevice?.characteristics?.[charUuid] ?? null;

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

    function renderCharacteristicInput(charUuid: string, charInfo: CharacteristicInfo) {
        if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_BOOLEAN) {
            return <CharacteristicBoolean charUuid={charUuid} charInfo={charInfo} onWrite={writeCharValue} />;
        }
        if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_UTF8S) {
            return (
                <CharacteristicUtf8
                    charUuid={charUuid}
                    charInfo={charInfo}
                    pendingValue={pendingValues[charUuid] ?? ''}
                    onChangeText={(uuid, text) => setPendingValues(prev => ({ ...prev, [uuid]: text }))}
                    onWrite={writeCharValue}
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
                    onWrite={writeCharValue}
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
                    onWrite={writeCharValue}
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

    const visibleServices = (selectedDevice?.services ?? [])
        .filter(service => service.uuid !== UUID_GENERIC_ATTRIBUTE_SERVICE && service.uuid !== UUID_GENERIC_ACCESS_SERVICE);

    return (
        <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
            <KeyboardAvoidingView
                style={styles.container}
                behavior={Platform.OS === 'ios' ? 'padding' : 'height'}
                keyboardVerticalOffset={50}>

                <View style={styles.header}>
                    <ThemedText type="heading">Controls</ThemedText>
                    <Badge
                        label={selectedDevice == null ? "NOT CONNECTED" : selectedDevice.name}
                        tone={selectedDevice == null ? 'danger' : 'success'}
                    />
                </View>

                {selectedDevice == null ? (
                    <EmptyState
                        icon="🕶️"
                        title="Not connected"
                        subtitle="Open the Connect tab to pair your glasses."
                        action={
                            <Link href="/(tabs)/bluetooth" asChild>
                                <AppButton title="Go to Connect" variant="primary" />
                            </Link>
                        }
                    />
                ) : (
                    <ScrollView contentContainerStyle={styles.scrollContent}>
                        {visibleServices.map((service, index) => {
                            const characteristics = Object.entries(selectedDevice?.characteristicsByService[service.uuid] ?? {})
                                .filter(([charUuid]) => charUuid !== UUID_ANIMATION_NAME_CHARACTERISTIC);

                            return (
                                <Card key={service.uuid + `-service-` + String(index)} style={styles.card}>
                                    <Section title={selectedDevice?.serviceDisplayNames?.[service.uuid] ?? getServiceName(service.uuid)}>
                                        {characteristics.map(([charUuid, charInfo], charIndex) => {
                                            const isMcuMgrCharacteristic = service.uuid === SMP_SERVICE_UUID && charUuid === SMP_CHARACTERISTIC_UUID;

                                            // Animated status color for this characteristic's label.
                                            const status = writeStatus[charUuid];
                                            const fadeValue = fadeAnims.current[charUuid];
                                            const statusColor = status === 'success' ? c.success
                                                : status === 'notification' ? c.info
                                                : c.danger;
                                            const labelColor = status && fadeValue
                                                ? fadeValue.interpolate({
                                                    inputRange: [0, 1],
                                                    outputRange: [c.textPrimary, statusColor],
                                                })
                                                : c.textPrimary;

                                            return (
                                                <React.Fragment key={`${service.uuid}-char-${charIndex}`}>
                                                    {charIndex > 0 && <Divider />}
                                                    <ListRow
                                                        label={charInfo.name ?? getCharacteristicName(charUuid)}
                                                        labelColor={labelColor}
                                                    >
                                                        {isMcuMgrCharacteristic && (
                                                            <Link href="/firmware-update-modal" asChild>
                                                                <AppButton title="Update" variant="secondary" />
                                                            </Link>
                                                        )}
                                                        {renderCharacteristicInput(charUuid, charInfo)}
                                                    </ListRow>
                                                </React.Fragment>
                                            );
                                        })}
                                    </Section>
                                </Card>
                            );
                        })}
                    </ScrollView>
                )}
            </KeyboardAvoidingView>
        </SafeAreaView>
    );
}

const styles = StyleSheet.create({
    container: {
        flex: 1,
        overflow: 'hidden',
    },
    header: {
        flexDirection: 'row',
        alignItems: 'center',
        justifyContent: 'space-between',
        gap: Spacing.sm,
        paddingHorizontal: Spacing.lg,
        paddingTop: Spacing.lg,
        paddingBottom: Spacing.md,
    },
    scrollContent: {
        paddingHorizontal: Spacing.lg,
        paddingBottom: Spacing.xxl,
        gap: Spacing.md,
    },
    card: {
        marginBottom: 0,
    },
});
