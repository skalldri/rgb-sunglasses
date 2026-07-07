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

    // Per-invocation generation token that guards against orphaned scans.
    // startBluetoothScan() awaits requestPermissions() (several native round-
    // trips), so the screen can lose focus AND regain it (a fast tab flip) while
    // a call is mid-await. A shared boolean can't tell "this invocation was
    // superseded" from "we're focused again" - the refocus resets it and the
    // stale invocation sails through, starting a scan with no cleanup bound to
    // it. react-native-ble-plx does not stop a prior scan when a new one starts,
    // so each orphaned scan permanently consumes one of Android's small number of
    // concurrent scan-client slots until the next
    // SCAN_FAILED_APPLICATION_REGISTRATION_FAILED. Instead, every focus and every
    // blur bumps this counter; each startBluetoothScan() call captures its value
    // and bails after any await (or in the scan callback) once the counter has
    // moved on - so only the newest invocation ever touches the scanner.
    const scanGenRef = useRef(0);

    // One automatic retry per focus for a scan that fails to START (most commonly
    // SCAN_FAILED_APPLICATION_REGISTRATION_FAILED / error code 6 - Android's
    // phone-wide scanner-client pool momentarily exhausted). Reset on every focus.
    const scanRetriedRef = useRef(false);
    // Handle for the retry setTimeout above, so the focus-effect cleanup can
    // cancel a pending retry - otherwise it fires into the next focus session and
    // starts a second concurrent scan (setDevices([]) also wipes the live scan's
    // results). null when no retry is scheduled.
    const retryTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

    /**
     *
     * @param mac De-duplicate devices
     */
    function isDuplicateDevice(allDevices: BleDevice[], newMac: string) {
        return allDevices.findIndex((d) => d.mac === newMac) >= 0;
    }

    async function startBluetoothScan(gen: number) {
        console.log('Starting Bluetooth scan...');
        setIsScanning(true);
        setDevices([]);

        // Defensive: dispose of any scan orphaned by a previous invocation that
        // resolved its permission check after the screen had already lost focus
        // (see scanGenRef above) - stopDeviceScan is a safe no-op if nothing
        // is currently scanning.
        bleManager.stopDeviceScan();

        try {
            const permissionsGranted = await requestPermissions();
            if (!permissionsGranted) {
                console.log('Bluetooth permissions denied');
                setIsScanning(false);
                return;
            }

            // The screen may have lost focus (and possibly regained it) while the
            // permission check above was in flight - the focus effect's cleanup
            // already ran and bumped scanGenRef, and could not stop a scan that
            // hadn't started yet. Bail out here instead of starting an orphaned
            // scan; if the screen refocused, a newer invocation now owns the
            // current generation and will start its own scan.
            if (scanGenRef.current !== gen) {
                return;
            }

            // startDeviceScan returns void (not a Promise) and delivers per-scan
            // errors to the callback below — not to the surrounding try/catch.
            bleManager.startDeviceScan(null, null, (error, device) => {
                // This invocation's scan was superseded (screen blurred/refocused)
                // while the scan was live. Stop it and stop processing callbacks so
                // we don't append devices into - or clear - a newer scan's results.
                if (scanGenRef.current !== gen) {
                    bleManager.stopDeviceScan();
                    return;
                }
                if (error) {
                    console.log('Scan error:', error);
                    // The scan is dead once the callback delivers an error (the
                    // library won't restart it). Stop cleanly, then either retry
                    // once (see scanRetriedRef above) or drop out of the scanning
                    // state so the UI shows the empty-state instead of an
                    // indefinite spinner over a scan that isn't running.
                    bleManager.stopDeviceScan();
                    if (!scanRetriedRef.current) {
                        scanRetriedRef.current = true;
                        console.log('Scan failed to start - retrying once in 2s...');
                        retryTimerRef.current = setTimeout(() => {
                            retryTimerRef.current = null;
                            // Only retry if this generation still owns the screen;
                            // the focus-effect cleanup also clearTimeout()s this, so
                            // this guard is belt-and-suspenders.
                            if (scanGenRef.current === gen) {
                                startBluetoothScan(gen);
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
            // Claim a fresh generation for this focus session; the cleanup below
            // bumps it again so any invocation still mid-await is left behind.
            const gen = ++scanGenRef.current;
            scanRetriedRef.current = false;
            startBluetoothScan(gen);

            return () => {
                // Invalidate this session's in-flight work (a pending permission
                // await, or the scan callback) so it can't start/keep a scan after
                // we've lost focus.
                scanGenRef.current++;
                // Cancel a pending failure-retry so it doesn't fire into the next
                // focus session and start a second concurrent scan.
                if (retryTimerRef.current) {
                    clearTimeout(retryTimerRef.current);
                    retryTimerRef.current = null;
                }
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
