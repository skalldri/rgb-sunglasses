import { useBluetooth } from "@/context/bluetooth-context";
import { useBleConnection } from "@/hooks/use-ble-connection";
import { useRouter } from "expo-router";
import { ActivityIndicator, Button, StyleSheet, View } from "react-native";
import { ThemedText } from "./themed-text";

interface Props {
    deviceName: string;
    macAddress: string;
}

export default function BluetoothDeviceListItem({ deviceName, macAddress }: Props) {
    const { selectedDevice, discoveryProgress } = useBluetooth();
    const { isConnecting, connect, disconnect } = useBleConnection(macAddress, deviceName);
    const router = useRouter();

    const isSelected = selectedDevice?.mac === macAddress;

    return (
        <View style={styles.outer}>
            <View style={styles.container}>
                <ThemedText style={styles.deviceName}>{deviceName}</ThemedText>
                <ThemedText style={styles.macAddress}>{macAddress}</ThemedText>
                <View style={styles.buttonContainer}>
                    <Button
                        title={isSelected ? "Disconnect" : "Connect"}
                        disabled={isConnecting}
                        onPress={async () => {
                            if (isSelected) {
                                await disconnect();
                            } else {
                                await connect();
                                router.navigate('/(tabs)/device-state');
                            }
                        }}
                    />
                    {isConnecting && !discoveryProgress && (
                        <View style={styles.loadingOverlay}>
                            <ActivityIndicator size="small" color="#fff" />
                        </View>
                    )}
                </View>
            </View>
            {isConnecting && discoveryProgress && (
                <View style={styles.progressContainer}>
                    <View style={styles.progressTrack}>
                        <View
                            style={[
                                styles.progressFill,
                                { width: `${Math.min(100, (discoveryProgress.current / Math.max(1, discoveryProgress.total)) * 100)}%` },
                            ]}
                        />
                    </View>
                    <ThemedText style={styles.progressLabel}>
                        {`Querying characteristics: ${discoveryProgress.current}/${discoveryProgress.total}`}
                    </ThemedText>
                </View>
            )}
        </View>
    );
}

const styles = StyleSheet.create({
    outer: {
        gap: 4,
    },
    container: {
        flexDirection: 'row',
        alignItems: 'center',
        justifyContent: 'space-between',
    },
    deviceName: {
        flex: 1,
    },
    macAddress: {
        flex: 1,
        fontSize: 12,
        opacity: 0.6,
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
    progressTrack: {
        height: 6,
        borderRadius: 3,
        backgroundColor: '#3a3a3a',
        overflow: 'hidden',
    },
    progressFill: {
        height: '100%',
        backgroundColor: '#4499ff',
        borderRadius: 3,
    },
    progressLabel: {
        fontSize: 11,
        opacity: 0.7,
    },
});