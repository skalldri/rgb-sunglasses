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
    const { selectedDevice } = useBluetooth();
    const { isConnecting, connect, disconnect } = useBleConnection(macAddress, deviceName);
    const router = useRouter();

    const isSelected = selectedDevice?.mac === macAddress;

    return (
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
                {isConnecting && (
                    <View style={styles.loadingOverlay}>
                        <ActivityIndicator size="small" color="#fff" />
                    </View>
                )}
            </View>
        </View>
    );
}

const styles = StyleSheet.create({
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
});