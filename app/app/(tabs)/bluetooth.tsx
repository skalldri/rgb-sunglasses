import BluetoothDeviceListItem from "@/components/bluetooth-device-list-item";
import { ThemedText } from "@/components/themed-text";
import { AppButton } from "@/components/ui/app-button";
import { Card } from "@/components/ui/card";
import { EmptyState } from "@/components/ui/empty-state";
import { Hero } from "@/components/ui/hero";
import { Screen } from "@/components/ui/screen";
import { Spacing } from "@/constants/theme";
import { useCallback, useRef, useState } from "react";
import { ActivityIndicator, StyleSheet, View } from 'react-native';

import { useBluetooth } from "@/context/bluetooth-context";
import { useAppUpdateCheck } from "@/hooks/use-app-update-check";
import { bleManager, requestPermissions } from "@/hooks/ble-manager";
import { useThemeColors } from "@/hooks/use-theme-color";
import { APP_SELF_UPDATE_SUPPORTED, getCurrentAppVersion } from "@/services/app-update";
import { Link, useFocusEffect } from "expo-router";
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
    const { info: appUpdate } = useAppUpdateCheck();

    // Guards against starting an orphaned scan: requestPermissions() below does
    // several async native round-trips, so the screen can lose focus (running
    // the useFocusEffect cleanup, which stops whatever scan is active *right
    // now*) before startDeviceScan() ever gets called. Without this check, that
    // startDeviceScan() call fires anyway once the permission check resolves,
    // with no cleanup left to ever stop it - and react-native-ble-plx does not
    // stop a prior scan when a new one starts, so each orphaned scan permanently
    // consumes one of Android's small number of concurrent scan-client slots
    // until the next SCAN_FAILED_APPLICATION_REGISTRATION_FAILED.
    const cancelledRef = useRef(false);

    // One automatic retry per focus for a scan that fails to START (most
    // commonly SCAN_FAILED_APPLICATION_REGISTRATION_FAILED / error code 6 -
    // Android's phone-wide scanner-client pool momentarily exhausted, observed
    // on shared hardware with several other BLE apps installed, and typically
    // transient as other apps' registrations churn). Before this, the error was
    // only console.logged: no scan was running, but the UI spun on "Scanning…"
    // forever. Reset on every focus so each visit to the screen gets one fresh
    // retry.
    const scanRetriedRef = useRef(false);

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

        // Defensive: dispose of any scan orphaned by a previous invocation that
        // resolved its permission check after the screen had already lost focus
        // (see cancelledRef above) - stopDeviceScan is a safe no-op if nothing
        // is currently scanning.
        bleManager.stopDeviceScan();

        try {
            const permissionsGranted = await requestPermissions();
            if (!permissionsGranted) {
                console.log('Bluetooth permissions denied');
                setIsScanning(false);
                return;
            }

            // The screen may have lost focus while the permission check above was
            // in flight - the focus effect's cleanup already ran and could not
            // stop a scan that hadn't started yet. Bail out here instead of
            // starting an orphaned scan with no cleanup bound to it.
            if (cancelledRef.current) {
                return;
            }

            // startDeviceScan returns void (not a Promise) and delivers per-scan
            // errors to the callback below — not to the surrounding try/catch.
            bleManager.startDeviceScan(null, null, (error, device) => {
                if (error) {
                    console.log('Scan error:', error);
                    // The scan is dead once the callback delivers an error (the
                    // library won't restart it). Stop cleanly, then either retry
                    // once (see scanRetriedRef above) or drop out of the scanning
                    // state so the UI shows the empty-state instead of an
                    // indefinite spinner over a scan that isn't running.
                    bleManager.stopDeviceScan();
                    if (!scanRetriedRef.current && !cancelledRef.current) {
                        scanRetriedRef.current = true;
                        console.log('Scan failed to start - retrying once in 2s...');
                        setTimeout(() => {
                            if (!cancelledRef.current) {
                                startBluetoothScan();
                            }
                        }, 2000);
                    } else {
                        setIsScanning(false);
                    }
                    return;
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
        } catch (error) {
            // Bluetooth may be unavailable: powered off, or — notably on iOS — the
            // Simulator, which has no BLE radio and rejects scan/query calls. Log it
            // and drop out of the scanning state instead of leaving an uncaught
            // promise rejection (which surfaces as a red LogBox in dev builds).
            // If startDeviceScan already succeeded before a later call (e.g.
            // connectedDevices) threw, stop it so we don't leave a scan running
            // while the UI shows scanning as stopped. stopDeviceScan is a safe no-op
            // if no scan is active.
            console.log('Bluetooth scan failed:', error);
            bleManager.stopDeviceScan();
            setIsScanning(false);
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
            cancelledRef.current = false;
            scanRetriedRef.current = false;
            startBluetoothScan();

            return () => {
                cancelledRef.current = true;
                stopBluetoothScan();
            };
        }, [])
    );

    return (
        <Screen scroll>
            <Hero title="RGB Sunglasses" subtitle="Connect over Bluetooth" emoji="🕶️" />

            {APP_SELF_UPDATE_SUPPORTED && appUpdate && (
                <Link href="/app-update-modal" asChild>
                    <AppButton
                        variant="primary"
                        title={`App update available: v${appUpdate.version} — tap to install`}
                        style={styles.updateBanner}
                    />
                </Link>
            )}

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

            {APP_SELF_UPDATE_SUPPORTED ? (
                <Link href="/app-update-modal" asChild>
                    <AppButton
                        variant="ghost"
                        title={`App v${getCurrentAppVersion()} • Check for updates`}
                        style={styles.versionRow}
                    />
                </Link>
            ) : (
                // iOS: in-app updates come from the App Store, so just show the
                // version (no link to the update modal).
                <ThemedText type="caption" style={[styles.versionRow, styles.versionLabel]}>
                    {`App v${getCurrentAppVersion()}`}
                </ThemedText>
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
    updateBanner: {
        marginBottom: Spacing.md,
    },
    versionRow: {
        marginTop: Spacing.xl,
    },
    versionLabel: {
        textAlign: 'center',
    },
});
