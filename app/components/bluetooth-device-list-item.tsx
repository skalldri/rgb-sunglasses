import { AppButton } from "@/components/ui/app-button";
import { ProgressBar } from "@/components/ui/progress-bar";
import { Spacing } from "@/constants/theme";
import { useBluetooth } from "@/context/bluetooth-context";
import { useBleConnection } from "@/hooks/use-ble-connection";
import { useThemeColors } from "@/hooks/use-theme-color";
import { useRouter } from "expo-router";
import { ActivityIndicator, Platform, StyleSheet, View } from "react-native";
import { ThemedText } from "./themed-text";

interface Props {
    deviceName: string;
    macAddress: string;
}

export default function BluetoothDeviceListItem({ deviceName, macAddress }: Props) {
    const { selectedDevice, discoveryProgress, reconnectingDevice } = useBluetooth();
    const { isConnecting, connect, disconnect, cancelReconnect } = useBleConnection(macAddress, deviceName);
    const router = useRouter();
    const c = useThemeColors();

    const isSelected = selectedDevice?.mac === macAddress;
    // An auto-reconnect loop is running for this device (issue #124). The button
    // becomes a tappable "Reconnecting…" that CANCELS the loop - reconnection
    // retries indefinitely, so the user needs an escape hatch. Note the loop's
    // connect attempts run in a context-level closure, not this row instance, so
    // local isConnecting is NOT reliable here - discoveryProgress/reconnecting
    // state from context drive the visuals instead.
    const isReconnecting = reconnectingDevice?.mac === macAddress;

    return (
        <View style={styles.outer}>
            <View style={styles.container}>
                <View style={styles.info}>
                    <ThemedText type="defaultSemiBold" numberOfLines={1}>{deviceName}</ThemedText>
                    {/* On iOS `device.id` is CoreBluetooth's opaque per-phone UUID (the real
                        MAC is never exposed there), which means nothing to a user — only
                        Android has a real MAC worth showing. */}
                    {Platform.OS !== "ios" && <ThemedText type="caption">{macAddress}</ThemedText>}
                </View>
                <View style={styles.buttonContainer}>
                    <AppButton
                        title={isSelected ? "Disconnect" : isReconnecting ? "Reconnecting…" : "Connect"}
                        variant={isSelected || isReconnecting ? "secondary" : "primary"}
                        disabled={isConnecting && !isReconnecting}
                        onPress={async () => {
                            if (isReconnecting) {
                                // Tap = cancel the auto-reconnect (issue #124).
                                cancelReconnect();
                            } else if (isSelected) {
                                await disconnect();
                            } else {
                                // Only navigate once the device is genuinely connected -
                                // connect() resolves false on failure (and shares the
                                // in-flight attempt's result on a duplicate tap), so a
                                // failed or deduped call no longer pushes an empty
                                // device-state screen.
                                if (await connect()) {
                                    router.navigate('/(tabs)/device-state');
                                }
                            }
                        }}
                    />
                    {(isConnecting || isReconnecting) && !discoveryProgress && (
                        // pointerEvents="none": the overlay must not swallow taps - while
                        // reconnecting, the button underneath is the cancel affordance.
                        <View style={styles.loadingOverlay} pointerEvents="none">
                            <ActivityIndicator size="small" color={c.onPrimary} />
                        </View>
                    )}
                </View>
            </View>
            {(isConnecting || isReconnecting) && discoveryProgress && (
                <View style={styles.progressContainer}>
                    <ProgressBar
                        progress={discoveryProgress.current / Math.max(1, discoveryProgress.total)}
                        label={`Querying characteristics: ${discoveryProgress.current}/${discoveryProgress.total}`}
                    />
                </View>
            )}
        </View>
    );
}

const styles = StyleSheet.create({
    outer: {
        gap: Spacing.sm,
    },
    container: {
        flexDirection: 'row',
        alignItems: 'center',
        justifyContent: 'space-between',
        gap: Spacing.md,
    },
    info: {
        flexShrink: 1,
        gap: 2,
    },
    buttonContainer: {
        position: 'relative',
    },
    loadingOverlay: {
        position: 'absolute',
        top: 0,
        left: 0,
        right: 0,
        bottom: 0,
        justifyContent: 'center',
        alignItems: 'center',
    },
    progressContainer: {
        gap: 2,
    },
});
