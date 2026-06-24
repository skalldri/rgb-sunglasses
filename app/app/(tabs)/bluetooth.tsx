import BluetoothDeviceListItem from "@/components/bluetooth-device-list-item";
import { ThemedText } from "@/components/themed-text";
import { Card } from "@/components/ui/card";
import { EmptyState } from "@/components/ui/empty-state";
import { Hero } from "@/components/ui/hero";
import { Screen } from "@/components/ui/screen";
import { Spacing } from "@/constants/theme";
import { useCallback, useState } from "react";
import { ActivityIndicator, StyleSheet, View } from 'react-native';

import { useBluetooth } from "@/context/bluetooth-context";
import { bleManager, requestPermissions } from "@/hooks/ble-manager";
import { useThemeColors } from "@/hooks/use-theme-color";
import { useFocusEffect } from "expo-router";
import { LogLevel } from "react-native-ble-plx";

// Set log level once at module load
if (__DEV__) bleManager.setLogLevel(LogLevel.Verbose);

type BleDevice = {
    name: string;
    mac: string;
};

export default function BluetoothScreen() {

    const { isScanning, setIsScanning } = useBluetooth();
    const [devices, setDevices] = useState<BleDevice[]>([]);
    const c = useThemeColors();

    /**
     *
     * @param mac De-duplicate devices
     */
    function isDuplicateDevice(allDevices: BleDevice[], newMac: string) {
        return allDevices.findIndex((d) => d.mac === newMac) >= 0;
    }

    async function startBluetoothScan() {
        console.log('Starting Bluetooth scan...');
        setIsScanning(true);
        setDevices([]);

        const permissionsGranted = await requestPermissions();
        if (!permissionsGranted) {
            console.log('Bluetooth permissions denied');
            setIsScanning(false);
            return;
        }

        await bleManager.startDeviceScan(null, null, (error, device) => {
            if (error) {
                console.log(error);
            }

            if (device) {
                if (device.localName?.includes("RGB Sunglasses")) {
                    console.log(`Found device: ${device.name ?? 'Unnamed'} (${device.id})`);

                    setDevices((prevDevices) => {

                        if (!isDuplicateDevice(prevDevices, device.id)) {
                            return [...prevDevices, { name: device.localName ?? 'Unnamed', mac: device.id }];
                        }

                        return prevDevices;
                    });
                }
            }
        });

        // Check if any devices are already paired with the OS with the "Core Config Service" UUID
        const connectedDevices = await bleManager.connectedDevices(["12345678-1234-5678-0001-56789abc0000"]);

        for (const device of connectedDevices) {
            console.log(`Already connected to device: ${device.name ?? 'Unnamed'} (${device.id})`);

            if (device.localName?.includes("RGB Sunglasses") || device.name?.includes("RGB Sunglasses")) {
                console.log(`Already connected to device: is an RGB Sunglasses!`);

                setDevices((prevDevices) => {

                    if (!isDuplicateDevice(prevDevices, device.id)) {
                        return [...prevDevices, { name: device.localName ?? device.name ?? 'Unnamed', mac: device.id }];
                    }

                    return prevDevices;
                });
            }
        }
    }

    function stopBluetoothScan() {
        console.log('Stopping Bluetooth scan...');
        bleManager.stopDeviceScan();
        setIsScanning(false);
    }

    // Start scanning when the screen is focused, stop when it loses focus
    useFocusEffect(
        useCallback(() => {
            startBluetoothScan();

            return () => {
                stopBluetoothScan();
            };
        }, [])
    );

    return (
        <Screen scroll>
            <Hero title="RGB Sunglasses" subtitle="Connect over Bluetooth" emoji="🕶️" />

            <View style={styles.statusRow}>
                <ThemedText type="overline">Nearby devices</ThemedText>
                {isScanning && <ActivityIndicator size="small" color={c.primary} />}
            </View>

            {devices.length === 0 ? (
                isScanning ? (
                    <ThemedText type="caption">Scanning…</ThemedText>
                ) : (
                    <EmptyState
                        icon="🔍"
                        title="No glasses found yet"
                        subtitle="Make sure your glasses are powered on and nearby."
                    />
                )
            ) : (
                <View style={styles.list}>
                    {devices.map(device => (
                        <Card key={device.mac}>
                            <BluetoothDeviceListItem
                                deviceName={device.name}
                                macAddress={device.mac}
                            />
                        </Card>
                    ))}
                </View>
            )}
        </Screen>
    );
}

const styles = StyleSheet.create({
    statusRow: {
        flexDirection: 'row',
        alignItems: 'center',
        justifyContent: 'space-between',
    },
    list: {
        gap: Spacing.md,
    },
});
