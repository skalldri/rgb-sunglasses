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
import { LogLevel, ScanMode } from "react-native-ble-plx";

// Set log level once at module load
if (__DEV__) bleManager.setLogLevel(LogLevel.Verbose);

type BleDevice = {
    name: string;
    mac: string;
};

// Android DE-DUPLICATES BLE scan reports: a device advertising continuously (the board
// does every ~100ms) is delivered to the scan callback ONCE, then suppressed and only
// re-reported sporadically (observed ~90-160s apart on a Pixel) - far longer than any
// advertising interval, and `allowDuplicates` is iOS-only in react-native-ble-plx. So we
// periodically RESTART the scan (RESCAN_INTERVAL_MS): a fresh startDeviceScan resets
// Android's per-scan "already reported" filter, re-reporting every currently-advertising
// device and refreshing its last-seen time. The prune TTL is kept comfortably longer than
// the restart interval so a present device (re-seen each restart) is never dropped, while
// one that stopped advertising (powered off, or connected elsewhere) ages out instead of
// lingering as "available" forever.
const RESCAN_INTERVAL_MS = 10_000;
const DEVICE_TTL_MS = 25_000;
const PRUNE_INTERVAL_MS = 2_000;

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

    // Per-device last-seen timestamps (mac -> epoch ms). Kept in a ref, NOT state, so the
    // high-frequency scan callback (fires on every advertisement) doesn't re-render the
    // list on each packet - only add/remove of a device changes `devices`. The prune
    // timer reads this to age out devices that have stopped advertising.
    const lastSeenRef = useRef<Record<string, number>>({});
    // Interval handle for the stale-device pruner; cleared on blur alongside the scan.
    const pruneTimerRef = useRef<ReturnType<typeof setInterval> | null>(null);
    // Interval handle for the periodic scan restart that defeats Android's report de-dup.
    const rescanTimerRef = useRef<ReturnType<typeof setInterval> | null>(null);

    // Remove devices we haven't seen advertise within DEVICE_TTL_MS. Returns the same
    // array reference when nothing changed so React skips the re-render.
    const pruneStaleDevices = useCallback(() => {
        const now = Date.now();
        setDevices(prev => {
            const fresh = prev.filter(d => now - (lastSeenRef.current[d.mac] ?? 0) <= DEVICE_TTL_MS);
            return fresh.length === prev.length ? prev : fresh;
        });
    }, []);

    /**
     *
     * @param mac De-duplicate devices
     */
    function isDuplicateDevice(allDevices: BleDevice[], newMac: string) {
        return allDevices.findIndex((d) => d.mac === newMac) >= 0;
    }

    // Register the native scan callback (no permission check, no list clear). Called for
    // the initial scan AND by the periodic rescan below, which stop+starts the scan to
    // reset Android's report de-dup so present devices are re-reported (refreshing their
    // lastSeen). scanMode LowLatency (highest duty cycle): react-native-ble-plx defaults to
    // LowPower, whose low duty cycle misses/delays a device that starts advertising AFTER
    // the scan began. LowLatency is Android's recommended mode for a foreground scan screen.
    function registerScanCallback(gen: number) {
            // startDeviceScan returns void (not a Promise) and delivers per-scan
            // errors to the callback below — not to the surrounding try/catch.
            bleManager.startDeviceScan(null, { scanMode: ScanMode.LowLatency }, (error, device) => {
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
                        // Refresh freshness on every advertisement (ref, not state - no
                        // re-render); the list only changes when a device is newly added.
                        lastSeenRef.current[device.id] = Date.now();

                        setDevices((prevDevices) => {

                            if (!isDuplicateDevice(prevDevices, device.id)) {
                                console.log(`Found device: ${device.name ?? 'Unnamed'} (${device.id})`);
                                return [...prevDevices, { name: device.localName ?? 'Unnamed', mac: device.id }];
                            }

                            return prevDevices;
                        });
                    }
                }
            });
    }

    async function startBluetoothScan(gen: number) {
        console.log('Starting Bluetooth scan...');
        setIsScanning(true);
        setDevices([]);
        lastSeenRef.current = {};

        // Defensive: dispose of any scan orphaned by a previous invocation that resolved
        // its permission check after the screen had already lost focus (see scanGenRef) -
        // stopDeviceScan is a safe no-op if nothing is currently scanning.
        bleManager.stopDeviceScan();

        try {
            const permissionsGranted = await requestPermissions();
            if (!permissionsGranted) {
                console.log('Bluetooth permissions denied');
                setIsScanning(false);
                return;
            }

            // The screen may have lost focus (and possibly regained it) while the
            // permission check above was in flight - bail out here instead of starting an
            // orphaned scan; if it refocused, a newer invocation now owns the generation.
            if (scanGenRef.current !== gen) {
                return;
            }

            registerScanCallback(gen);

            // Check if any devices are already paired with the OS with the "Core Config Service" UUID
            const connectedDevices = await bleManager.connectedDevices(["12345678-1234-5678-0001-56789abc0000"]);

            for (const device of connectedDevices) {
                console.log(`Already connected to device: ${device.name ?? 'Unnamed'} (${device.id})`);

                if (device.localName?.includes("RGB Sunglasses") || device.name?.includes("RGB Sunglasses")) {
                    console.log(`Already connected to device: is an RGB Sunglasses!`);

                    // Seed freshness so this OS-connected board gets a normal TTL window
                    // (the scan callback won't refresh it - it isn't advertising while
                    // connected). Re-seeded on each scan start; it also shows on the
                    // Controls screen once connected, so pruning it here after the TTL is
                    // acceptable rather than a regression.
                    lastSeenRef.current[device.id] = Date.now();

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
            // Age out devices that stop advertising while the screen stays focused (the
            // scan-restart that used to be the only list-clear happens only on focus/blur).
            pruneTimerRef.current = setInterval(pruneStaleDevices, PRUNE_INTERVAL_MS);
            // Periodically restart the scan to defeat Android's report de-dup: a fresh
            // startDeviceScan re-reports every currently-advertising device (refreshing its
            // lastSeen) so a still-present board isn't pruned during Android's long gaps
            // between duplicate reports. Does NOT clear the list -> no flicker. Guarded on
            // the generation so it goes quiet the moment the screen is superseded.
            rescanTimerRef.current = setInterval(() => {
                if (scanGenRef.current !== gen) {
                    return;
                }
                bleManager.stopDeviceScan();
                registerScanCallback(gen);
            }, RESCAN_INTERVAL_MS);

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
                if (pruneTimerRef.current) {
                    clearInterval(pruneTimerRef.current);
                    pruneTimerRef.current = null;
                }
                if (rescanTimerRef.current) {
                    clearInterval(rescanTimerRef.current);
                    rescanTimerRef.current = null;
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
