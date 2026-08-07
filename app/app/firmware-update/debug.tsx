import { ThemedText } from '@/components/themed-text';
import { AppButton } from '@/components/ui/app-button';
import { Badge } from '@/components/ui/badge';
import { Card } from '@/components/ui/card';
import { Divider } from '@/components/ui/divider';
import { ProgressBar } from '@/components/ui/progress-bar';
import { IconSymbol } from '@/components/ui/icon-symbol';
import { UUID_MCUBOOT_INFO_SERVICE } from '@/constants/bluetooth';
import { Spacing } from '@/constants/theme';
import { useBluetooth } from '@/context/bluetooth-context';
import { useMcuMgrClientContext } from '@/context/mcumgr-client-context';
import { useThemeColors } from '@/hooks/use-theme-color';
import { decodeUtf8FromBase64 } from '@/services/ble-value-codec';
import {
    compareVersions,
    extractBoardRevision,
    fetchLatestMcubootRelease,
    findAssetForBoard,
    GitHubAsset,
    parseVersionFromTag,
} from '@/services/github-releases';
import {
    McubootUpdaterClient,
    McubootPackageInfo,
    McubootUpdaterState,
    MCUBOOT_CHUNK_SIZE,
    parseMcubootPackage,
} from '@/services/mcuboot-updater-client';
import { formatBytes, formatHash, ImageSlot, SlotInfoResponse } from '@/services/mcumgr';
import * as DocumentPicker from 'expo-document-picker';
import * as LegacyFS from 'expo-file-system/legacy';
import { File } from 'expo-file-system/next';
import { useRouter } from 'expo-router';
import React, { useCallback, useEffect, useRef, useState } from 'react';
import { ActivityIndicator, Alert, Pressable, ScrollView, StyleSheet, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

// ============================================================================
// FW Update Debug — the low-level page
//
// This is the original all-in-one firmware screen, kept for development and
// recovery work. The everyday path now lives in `index.tsx` (landing) and
// `flow.tsx` (guided update); what stayed here is everything diagnostic:
// the raw slot table, mark-for-test, manual reset/erase, slot info, and the
// MCUboot bootloader updater.
//
// It deliberately does NOT upload firmware itself any more. The one upload
// implementation lives in `hooks/use-firmware-update-flow.ts`, which stages for
// test and confirms only after verifying the reboot; a second copy here would
// have kept the old mark-permanent-immediately behaviour alive.
// ============================================================================

function flagTone(flag: string): 'success' | 'info' | 'neutral' {
    if (flag === 'Active' || flag === 'Confirmed' || flag === 'Bootable') return 'success';
    if (flag === 'Pending' || flag === 'Permanent') return 'info';
    return 'neutral';
}

function base64ToBytes(base64Data: string): Uint8Array {
    const binary = atob(base64Data);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
    return bytes;
}

export default function FirmwareUpdateDebug() {
    const { selectedDevice, reconnectingDevice } = useBluetooth();
    // The client is owned by the route-group provider, not by this screen — see
    // context/mcumgr-client-context.tsx for why there must only ever be one.
    const { client, isInitializing, error: initError } = useMcuMgrClientContext();
    const c = useThemeColors();
    const router = useRouter();
    const [imageState, setImageState] = useState<ImageSlot[]>([]);
    const [status, setStatus] = useState<string>('');
    const [error, setError] = useState<string>('');
    const [slotInfo, setSlotInfo] = useState<SlotInfoResponse | null>(null);

    // Board detection (still needed here for the MCUboot release lookup below)
    const [boardRevision, setBoardRevision] = useState<string | null>(null);
    const [boardDetectionError, setBoardDetectionError] = useState<string>('');

    // Bootloader update
    const blUpdaterRef = useRef<McubootUpdaterClient | null>(null);
    const [blPackage, setBlPackage] = useState<McubootPackageInfo | null>(null);
    const [blStatus, setBlStatus] = useState<McubootUpdaterState>(McubootUpdaterState.LOCKED);
    const [blProgress, setBlProgress] = useState(0);
    const [blError, setBlError] = useState<string>('');
    const [blFlashUnlocked, setBlFlashUnlocked] = useState(false);
    const [blRebooting, setBlRebooting] = useState(false);

    // Bootloader GitHub update check
    type McubootUpdateCheckState = 'idle' | 'checking' | 'upToDate' | 'updateAvailable' | 'error';
    const [mcubootUpdateCheckState, setMcubootUpdateCheckState] = useState<McubootUpdateCheckState>('idle');
    const [mcubootLatestAsset, setMcubootLatestAsset] = useState<GitHubAsset | null>(null);
    const [mcubootLatestVersion, setMcubootLatestVersion] = useState<string>('');
    const [mcubootUpdateCheckError, setMcubootUpdateCheckError] = useState<string>('');
    const [isDownloadingBootloader, setIsDownloadingBootloader] = useState(false);
    const [bootloaderDownloadProgress, setBootloaderDownloadProgress] = useState<number>(0);

    // Read-only bootloader device info characteristics (e.g. "MCUboot Version"), sourced from the
    // same BluetoothContext data device-state/index.tsx used to show inline in the Controls tab —
    // moved here since it's more relevant next to the bootloader update controls (issue #76).
    const mcubootInfoChars = selectedDevice?.characteristicsByService?.[UUID_MCUBOOT_INFO_SERVICE];
    const mcubootDeviceInfo = Object.entries(mcubootInfoChars ?? {})
        .filter(([, charInfo]) => charInfo.name != null)
        .map(([charUuid, charInfo]) => {
            let displayValue = '—';
            if (charInfo.value) {
                try {
                    displayValue = decodeUtf8FromBase64(charInfo.value).replace(/\0/g, '');
                } catch {
                    displayValue = '—';
                }
            }
            return { charUuid, name: charInfo.name as string, value: displayValue };
        });
    const currentMcubootVersion = mcubootDeviceInfo.find(info => info.name === 'MCUboot Version')?.value ?? null;

    // Initialize (and tear down) the MCUboot updater BLE client alongside the MCUmgr client.
    useEffect(() => {
        const device = selectedDevice?.device ?? null;
        if (!device) {
            blUpdaterRef.current?.destroy();
            blUpdaterRef.current = null;
            setBlStatus(McubootUpdaterState.LOCKED);
            setBlProgress(0);
            setBlError('');
            setBlPackage(null);
            setBlFlashUnlocked(false);
            // Deliberately NOT resetting blRebooting here: this branch runs on the disconnect
            // that immediately follows a self-requested reboot (by design, ~200ms after the
            // write), so clearing it here would hide the "reconnect" message before the user
            // ever sees it. It's cleared once the device reconnects and reports fresh status.
            return;
        }

        const updater = new McubootUpdaterClient();
        blUpdaterRef.current = updater;

        // Registered before initialize() so the initial status read it performs on connect
        // (not just the state-change notification stream) reaches this handler instead of
        // being silently dropped.
        updater.onStatusChanged(s => {
            setBlStatus(s.state);
            setBlProgress(s.progress);
            setBlFlashUnlocked(s.flashUnlocked);
            // Any status update means the device is alive — clear rebooting state.
            setBlRebooting(false);
            if (s.state === McubootUpdaterState.ERROR) {
                setBlError(`Updater error (code ${s.errorCode})`);
            }
        });

        updater.initialize(device).catch(err => {
            // Service not present on this board variant — not an error worth surfacing prominently.
            // Clear the ref so the bootloader section stays hidden for unsupported devices.
            console.log('MCUboot updater service unavailable:', err?.message ?? err);
            blUpdaterRef.current = null;
        });

        return () => {
            updater.destroy();
        };
    }, [selectedDevice?.device]);

    const refreshImageState = useCallback(async () => {
        if (!client) return;

        try {
            setStatus('Fetching image state...');
            const state = await client.getImageState();
            setImageState(state.images);
            setStatus('');
        } catch (e: unknown) {
            const errorMessage = e instanceof Error ? e.message : String(e);
            setError(`Failed to get image state: ${errorMessage}`);
        }
    }, [client]);

    const refreshSlotInfo = useCallback(async () => {
        if (!client) return;

        try {
            setStatus('Fetching slot info...');
            const info = await client.getSlotInfo();
            // Only set slot info if it has the images property
            if (info && info.images && Array.isArray(info.images)) {
                setSlotInfo(info);
            } else {
                console.log('Slot info response missing images array:', info);
                setSlotInfo(null);
            }
            setStatus('');
        } catch (e: unknown) {
            // Slot info command may not be supported on all devices
            console.log('Slot info not available:', e);
            setSlotInfo(null);
        }
    }, [client]);

    // Fetch initial state when client becomes available, then detect board revision.
    // All three SMP calls are sequenced to avoid concurrent requests (McuMgrClient
    // only supports one pending request at a time).
    useEffect(() => {
        if (!client) return;

        async function fetchInitialState() {
            await refreshImageState();
            await refreshSlotInfo();

            try {
                const boardName = await client!.getOsInfo('i');
                const revision = extractBoardRevision(boardName);
                if (revision) {
                    setBoardRevision(revision);
                } else {
                    setBoardDetectionError(`Unknown board: ${boardName}`);
                }
            } catch (e: unknown) {
                setBoardDetectionError(
                    `Board detection failed: ${e instanceof Error ? e.message : String(e)}`
                );
            }
        }

        fetchInitialState();
    }, [client, refreshImageState, refreshSlotInfo]);

    // NOTE: initError is deliberately NOT copied into `error` here. It is
    // 'No device connected' whenever the link is down - including during a perfectly
    // normal reboot - and the old code assigned it without ever clearing it, so a
    // stale red banner outlived the reconnect. It is rendered as neutral status below.

    // Reset board-detection/update-check state when the client disconnects (e.g. the device
    // reboots to apply an update via "Reset Device"). Without this, a stale "Update Available"
    // card (with now-inaccurate Current/Latest versions) can keep showing - harmless since the
    // Download button is already disabled while !client, but confusing - until the user backs
    // out of this screen and reopens it, which is the only thing that currently remounts this
    // component and re-derives this state from scratch. Resetting here means a fresh
    // board-detection + GitHub check (not a stale local recomputation) runs as soon as the
    // device reconnects, without requiring that manual round-trip.
    useEffect(() => {
        if (client) return;

        setBoardRevision(null);
        setBoardDetectionError('');
        setMcubootUpdateCheckState('idle');
        setMcubootLatestAsset(null);
        setMcubootLatestVersion('');
        setMcubootUpdateCheckError('');
    }, [client]);

    // Check GitHub for the latest standalone MCUboot release. Proto0-only, like the bootloader
    // updater service itself (fw/Kconfig's APP_MCUBOOT_UPDATER), and only once the device's own
    // current MCUboot version is known (read from the Device Info characteristics above).
    useEffect(() => {
        if (boardRevision !== 'proto0' || !currentMcubootVersion || mcubootUpdateCheckState !== 'idle') return;

        async function checkForMcubootUpdates() {
            setMcubootUpdateCheckState('checking');
            try {
                const release = await fetchLatestMcubootRelease('skalldri', 'rgb-sunglasses');
                if (!release) {
                    setMcubootUpdateCheckState('upToDate');
                    return;
                }

                const asset = findAssetForBoard(release.assets, boardRevision!, '.bin');
                if (!asset) {
                    throw new Error(`No MCUboot asset found for board: ${boardRevision}`);
                }

                const githubVersion = parseVersionFromTag(release.tag_name);
                const cmp = compareVersions(currentMcubootVersion!, githubVersion);

                setMcubootLatestAsset(asset);
                setMcubootLatestVersion(githubVersion);
                setMcubootUpdateCheckState(cmp < 0 ? 'updateAvailable' : 'upToDate');
            } catch (e: unknown) {
                setMcubootUpdateCheckError(e instanceof Error ? e.message : String(e));
                setMcubootUpdateCheckState('error');
            }
        }

        checkForMcubootUpdates();
        // mcubootUpdateCheckState is read only as a run-once idle guard, not to compute
        // the result; it's deliberately not a dependency so the check fires once per
        // board/version change rather than re-firing on its own state transitions.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [boardRevision, currentMcubootVersion]);

    /**
     * Sideload a .zip. Hands off to the guided flow rather than uploading here: the
     * flow is the single implementation of upload -> stage-for-test -> reboot ->
     * verify -> confirm, and a second copy on this page would have kept the old
     * mark-permanent-immediately behaviour alive alongside it.
     */
    async function handleSelectFirmwarePackage() {
        try {
            setError('');
            const result = await DocumentPicker.getDocumentAsync({
                type: 'application/zip',
                copyToCacheDirectory: true,
            });
            if (result.canceled || !result.assets?.[0]) return;

            const file = result.assets[0];
            router.push({
                pathname: '/firmware-update/flow',
                params: { source: 'file', uri: file.uri, name: file.name },
            });
        } catch (e: any) {
            setError(`Failed to load firmware package: ${e.message}`);
        }
    }

    async function handlePrepareDevice() {
        const updater = blUpdaterRef.current;
        if (!updater) return;

        setBlError('');
        try {
            await updater.requestUpdaterReboot();
            setBlRebooting(true);
        } catch (e: any) {
            setBlError(`Failed to prepare device: ${e.message}`);
            setBlRebooting(false);
        }
    }

    async function handleSelectBootloaderPackage() {
        try {
            setBlError('');
            const result = await DocumentPicker.getDocumentAsync({
                copyToCacheDirectory: true,
            });
            if (result.canceled || !result.assets?.[0]) return;

            const file = result.assets[0];
            const fileRef = new File(file.uri);
            const base64Data = await fileRef.base64();

            const pkg = parseMcubootPackage(base64ToBytes(base64Data));
            setBlPackage(pkg);
        } catch (e: any) {
            setBlError(`Failed to load package: ${e.message}`);
        }
    }

    async function handleDownloadBootloaderUpdate() {
        if (!mcubootLatestAsset) return;

        setIsDownloadingBootloader(true);
        setBootloaderDownloadProgress(0);
        setBlError('');

        const destUri = (LegacyFS.cacheDirectory ?? '') + 'mcuboot-update.bin';

        try {
            const task = LegacyFS.createDownloadResumable(
                mcubootLatestAsset.browser_download_url,
                destUri,
                {},
                ({ totalBytesWritten, totalBytesExpectedToWrite }: { totalBytesWritten: number; totalBytesExpectedToWrite: number }) => {
                    if (totalBytesExpectedToWrite > 0) {
                        setBootloaderDownloadProgress(
                            Math.round((totalBytesWritten / totalBytesExpectedToWrite) * 100)
                        );
                    }
                }
            );

            const result = await task.downloadAsync();
            if (!result) {
                throw new Error('Download was cancelled');
            }

            const fileRef = new File(result.uri);
            const base64Data = await fileRef.base64();
            const pkg = parseMcubootPackage(base64ToBytes(base64Data));
            setBlPackage(pkg);
        } catch (e: any) {
            setBlError(`Download failed: ${e.message}`);
        } finally {
            setIsDownloadingBootloader(false);
            setBootloaderDownloadProgress(0);
        }
    }

    async function handleStartBootloaderUpdate() {
        const updater = blUpdaterRef.current;
        if (!updater || !blPackage) return;

        // First confirmation: general risk warning
        const confirmed1 = await new Promise<boolean>(resolve => {
            Alert.alert(
                'Update MCUboot Bootloader?',
                'This will replace the MCUboot bootloader. Power loss during flashing may ' +
                'require J-Link recovery. Only proceed if you understand the risk.',
                [
                    { text: 'Cancel', style: 'cancel', onPress: () => resolve(false) },
                    { text: 'Continue', onPress: () => resolve(true) },
                ]
            );
        });
        if (!confirmed1) return;

        setBlError('');

        try {
            // Unlock the updater (LOCKED → IDLE)
            await updater.unlock();

            // Begin upload — this erases the staging partition (~2-3 s)
            await updater.beginUpload(blPackage.payload.length);

            // Stream the full package (header + payload) in chunks.
            // The header is the first 16 bytes already embedded in blPackage.payload's
            // parent array — rebuild the full package bytes here.
            const header = new Uint8Array(16);
            const hdrView = new DataView(header.buffer);
            hdrView.setUint32(0, 0x424D5247, true);          // magic
            header[4] = blPackage.major;
            header[5] = blPackage.minor;
            hdrView.setUint16(6, blPackage.revision, true);
            hdrView.setUint32(8, blPackage.payloadSize, true);
            hdrView.setUint32(12, blPackage.crc32, true);

            const fullPackage = new Uint8Array(16 + blPackage.payload.length);
            fullPackage.set(header, 0);
            fullPackage.set(blPackage.payload, 16);

            for (let offset = 0; offset < fullPackage.length; offset += MCUBOOT_CHUNK_SIZE) {
                const chunk = fullPackage.slice(offset, offset + MCUBOOT_CHUNK_SIZE);
                await updater.sendChunk(chunk);
                // Progress 0-80% maps to transfer phase
                setBlProgress(Math.round((offset / fullPackage.length) * 80));
            }

            // Validate — firmware computes CRC32 over the staged data
            await updater.validate();
            setBlProgress(90);

            // Second confirmation: final irreversible step
            const confirmed2 = await new Promise<boolean>(resolve => {
                Alert.alert(
                    'Flash Bootloader Now?',
                    `Validation passed (v${blPackage.major}.${blPackage.minor}.${blPackage.revision}, ` +
                    `${blPackage.payloadSize} bytes). Tap "Flash" to write and reboot. ` +
                    'The device will be unreachable for ~15 seconds.',
                    [
                        { text: 'Cancel', style: 'cancel', onPress: () => resolve(false) },
                        { text: 'Flash', style: 'destructive', onPress: () => resolve(true) },
                    ]
                );
            });

            if (!confirmed2) {
                await updater.abort();
                return;
            }

            // Commit — flashes internal flash and reboots (BLE link will drop)
            await updater.commit();
            setBlProgress(100);
            setBlPackage(null);
        } catch (e: any) {
            setBlError(e.message ?? 'Bootloader update failed');
        }
    }

    async function handleConfirmImage(hash: Uint8Array) {
        if (!client) return;

        try {
            setStatus('Confirming image...');
            await client.setImageState(hash, false); // Mark for test
            await refreshImageState();
        } catch (e: any) {
            setError(`Failed to confirm: ${e.message}`);
        }
    }

    async function handleReset() {
        if (!client) return;

        try {
            setStatus('Resetting device...');
            await client.reset();
            setStatus('Device is resetting. You may need to reconnect.');
        } catch {
            // Expected - device resets before responding
            setStatus('Device is resetting. You may need to reconnect.');
        }
    }

    async function handleEraseSlot() {
        if (!client) return;

        try {
            setStatus('Erasing secondary slot (this may take a while)...');
            await client.eraseImage(1);
            setStatus('Erase complete');
            await refreshImageState();
        } catch (e: any) {
            setError(`Erase failed: ${e.message}`);
        }
    }


    function renderImageSlot(slot: ImageSlot, index: number) {
        const flags = [];
        if (slot.active) flags.push('Active');
        if (slot.pending) flags.push('Pending');
        if (slot.confirmed) flags.push('Confirmed');
        if (slot.permanent) flags.push('Permanent');
        if (slot.bootable) flags.push('Bootable');

        return (
            <Card key={index} style={styles.cardSpacing}>
                <ThemedText style={styles.slotTitle}>
                    Image {slot.image ?? 0} / Slot {slot.slot}
                </ThemedText>
                <ThemedText type="caption">
                    Version: {slot.version}
                </ThemedText>
                <ThemedText type="caption">
                    Hash: {formatHash(slot.hash)}
                </ThemedText>
                {flags.length > 0 && (
                    <View style={styles.flagRow}>
                        {flags.map(flag => (
                            <Badge key={flag} label={flag} tone={flagTone(flag)} />
                        ))}
                    </View>
                )}
                {slot.hash && !slot.pending && slot.slot === 1 && (
                    <AppButton
                        title="Mark for Test"
                        variant="secondary"
                        style={styles.slotButton}
                        onPress={() => handleConfirmImage(slot.hash!)}
                    />
                )}
            </Card>
        );
    }

    function renderBootloaderSection() {
        const updater = blUpdaterRef.current;
        const isActive = blStatus === McubootUpdaterState.ERASING   ||
                         blStatus === McubootUpdaterState.RECEIVING  ||
                         blStatus === McubootUpdaterState.VALIDATING ||
                         blStatus === McubootUpdaterState.FLASHING;
        const isDone   = blStatus === McubootUpdaterState.DONE;

        const stateLabel: Record<McubootUpdaterState, string> = {
            [McubootUpdaterState.LOCKED]:     'Locked',
            [McubootUpdaterState.IDLE]:       'Ready',
            [McubootUpdaterState.ERASING]:    'Erasing staging area…',
            [McubootUpdaterState.RECEIVING]:  'Receiving binary…',
            [McubootUpdaterState.VALIDATING]: 'Validating CRC32…',
            [McubootUpdaterState.VALIDATED]:  'Validated',
            [McubootUpdaterState.FLASHING]:   'Flashing internal flash…',
            [McubootUpdaterState.DONE]:       'Done — device rebooting',
            [McubootUpdaterState.ERROR]:      'Error',
        };

        function renderMcubootAutoUpdateSection() {
            if (isDownloadingBootloader) {
                return (
                    <Card style={styles.updateCard}>
                        <ThemedText type="overline">Downloading Bootloader Update...</ThemedText>
                        <ProgressBar progress={bootloaderDownloadProgress / 100} label={`${bootloaderDownloadProgress}%`} height={12} />
                    </Card>
                );
            }

            if (mcubootUpdateCheckState === 'checking') {
                return (
                    <Card style={styles.updateCard}>
                        <ActivityIndicator size="small" color={c.primary} />
                        <ThemedText type="caption" style={styles.status}>Checking for bootloader updates...</ThemedText>
                    </Card>
                );
            }

            if (mcubootUpdateCheckState === 'error') {
                return (
                    <Card style={styles.updateCard}>
                        <ThemedText style={[styles.updateCardError, { color: c.danger }]}>
                            Bootloader update check failed: {mcubootUpdateCheckError}
                        </ThemedText>
                    </Card>
                );
            }

            if (mcubootUpdateCheckState === 'updateAvailable' && mcubootLatestAsset) {
                return (
                    <Card style={[styles.updateCard, { borderColor: c.success }]}>
                        <ThemedText type="overline" style={styles.sectionTitle}>
                            Bootloader Update Available
                        </ThemedText>
                        <ThemedText type="caption">Current: v{currentMcubootVersion}</ThemedText>
                        <ThemedText type="caption">Latest: v{mcubootLatestVersion}</ThemedText>
                        <View style={styles.buttonRow}>
                            <AppButton
                                title="Download Update"
                                variant="primary"
                                style={styles.rowButton}
                                onPress={handleDownloadBootloaderUpdate}
                                disabled={isActive}
                            />
                        </View>
                    </Card>
                );
            }

            return null;
        }

        return (
            <>
                <ThemedText type="overline" style={styles.sectionTitle}>
                    Bootloader Update (Advanced)
                </ThemedText>

                {mcubootDeviceInfo.length > 0 && (
                    <Card style={styles.cardSpacing}>
                        {mcubootDeviceInfo.map((info, index) => (
                            <React.Fragment key={info.charUuid}>
                                {index > 0 && <Divider />}
                                <View style={styles.infoRow}>
                                    <ThemedText style={styles.infoLabel}>{info.name}</ThemedText>
                                    <ThemedText style={styles.infoValue}>{info.value}</ThemedText>
                                </View>
                            </React.Fragment>
                        ))}
                    </Card>
                )}

                {renderMcubootAutoUpdateSection()}

                <Card style={[styles.updateCard, { borderColor: c.warning, borderWidth: 1 }]}>
                    <ThemedText type="caption" style={{ color: c.warning, textAlign: 'center' }}>
                        ⚠ Power loss during flashing can require J-Link recovery
                    </ThemedText>
                </Card>

                {blError ? (
                    <ThemedText style={[styles.error, { color: c.danger }]}>{blError}</ThemedText>
                ) : null}

                {(isActive || isDone) && (
                    <View style={styles.progressWrap}>
                        <ThemedText type="caption" style={styles.status}>
                            {stateLabel[blStatus]}
                        </ThemedText>
                        <ProgressBar progress={blProgress / 100} label={`${blProgress}%`} height={12} />
                    </View>
                )}

                {isDone && (
                    <ThemedText type="caption" style={[styles.status, { color: c.success }]}>
                        Bootloader updated. Reconnect after ~15 seconds.
                    </ThemedText>
                )}

                {blRebooting && (
                    <ThemedText type="caption" style={[styles.status, { color: c.warning }]}>
                        Device is rebooting — please reconnect after ~15 seconds
                    </ThemedText>
                )}

                {!blFlashUnlocked && !blRebooting && !isActive && !isDone && (
                    <Card style={styles.cardSpacing}>
                        <ThemedText type="caption" style={{ textAlign: 'center' }}>
                            Flash is protected. Tap &quot;Prepare Device&quot; to reboot into updater mode (unlocks flash).
                        </ThemedText>
                        <AppButton
                            title="Prepare Device"
                            variant="secondary"
                            style={{ marginTop: 8 }}
                            onPress={handlePrepareDevice}
                            disabled={!updater}
                        />
                    </Card>
                )}

                {blFlashUnlocked && !isActive && !isDone && blPackage && (
                    <Card style={styles.cardSpacing}>
                        <ThemedText style={styles.slotTitle}>
                            Package loaded
                        </ThemedText>
                        <ThemedText type="caption">
                            Version: {blPackage.major}.{blPackage.minor}.{blPackage.revision}
                        </ThemedText>
                        <ThemedText type="caption">
                            Size: {formatBytes(blPackage.payloadSize)}
                        </ThemedText>
                        <ThemedText type="caption">
                            CRC32: 0x{blPackage.crc32.toString(16).padStart(8, '0')}
                        </ThemedText>
                    </Card>
                )}

                {!isActive && !isDone && blFlashUnlocked && !blPackage && (
                    <ThemedText type="caption" style={[styles.status, { color: c.success }]}>
                        Flash is unlocked — select a package to flash
                    </ThemedText>
                )}

                <View style={styles.buttonRow}>
                    <AppButton
                        title="Select .bin Package"
                        variant="secondary"
                        style={styles.rowButton}
                        onPress={handleSelectBootloaderPackage}
                        disabled={isActive || !updater || !blFlashUnlocked}
                    />
                    {blPackage && blFlashUnlocked && (
                        <AppButton
                            title="Flash Bootloader"
                            variant="danger"
                            style={styles.rowButton}
                            onPress={handleStartBootloaderUpdate}
                            disabled={isActive || !updater}
                        />
                    )}
                </View>
            </>
        );
    }

    if (isInitializing) {
        return (
            <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
                <ActivityIndicator size="large" color={c.primary} />
                <ThemedText type="caption" style={styles.status}>{status || 'Initializing...'}</ThemedText>
            </SafeAreaView>
        );
    }

    return (
        <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
            <View style={styles.header}>
                <Pressable
                    onPress={() => router.back()}
                    accessibilityRole="button"
                    accessibilityLabel="Back to firmware update"
                    hitSlop={8}
                    style={styles.backButton}>
                    <IconSymbol name="chevron.left" size={22} color={c.primary} />
                    <ThemedText style={{ color: c.primary }}>Firmware</ThemedText>
                </Pressable>
            </View>

            {/* A real failure. Note this is NOT where "no device" goes - that is
                connection status, rendered neutrally just below. */}
            {error ? (
                <ThemedText style={[styles.error, { color: c.danger }]}>{error}</ThemedText>
            ) : null}

            {boardDetectionError ? (
                <ThemedText type="caption" style={styles.status}>{boardDetectionError}</ThemedText>
            ) : null}

            {!selectedDevice ? (
                <ThemedText type="caption" style={styles.status}>
                    {reconnectingDevice
                        ? `Reconnecting to ${reconnectingDevice.name}…`
                        : initError || 'No device connected'}
                </ThemedText>
            ) : null}

            {status ? (
                <ThemedText type="caption" style={styles.status}>{status}</ThemedText>
            ) : null}

            <ScrollView style={styles.scrollView} contentContainerStyle={styles.scrollContent}>
                    <>
                        <ThemedText type="title" style={styles.sectionTitle}>
                            FW Update Debug
                        </ThemedText>

                        <ThemedText type="overline" style={styles.sectionTitle}>
                            Current Images
                        </ThemedText>

                        {imageState.length === 0 ? (
                            <ThemedText type="caption" style={styles.noImages}>No images found</ThemedText>
                        ) : (
                            imageState.map(renderImageSlot)
                        )}

                        <View style={styles.buttonRow}>
                            <AppButton
                                title="Refresh"
                                variant="secondary"
                                style={styles.rowButton}
                                onPress={() => {
                                    refreshImageState();
                                    refreshSlotInfo();
                                }}
                                disabled={!client}
                            />
                        </View>

                        {slotInfo?.images && slotInfo.images.length > 0 && (
                            <>
                                <ThemedText type="overline" style={styles.sectionTitle}>
                                    Slot Info
                                </ThemedText>
                                {slotInfo.images.map((imageInfo, idx) => (
                                    <Card key={idx} style={styles.cardSpacing}>
                                        <ThemedText style={styles.slotTitle}>
                                            Image {imageInfo.image}: {imageInfo.image === 0 ? 'App Firmware' : 'Radio Firmware'}
                                        </ThemedText>
                                        {imageInfo.max_image_size && (
                                            <ThemedText type="caption">
                                                Max Size: {formatBytes(imageInfo.max_image_size)}
                                            </ThemedText>
                                        )}
                                        {imageInfo.slots?.map((slot, slotIdx) => (
                                            <ThemedText key={slotIdx} type="caption">
                                                Slot {slot.slot}: {formatBytes(slot.size)}
                                                {slot.upload_image_id !== undefined && ` (upload target: image ${slot.upload_image_id})`}
                                            </ThemedText>
                                        ))}
                                    </Card>
                                ))}
                            </>
                        )}

                        <ThemedText type="overline" style={styles.sectionTitle}>
                            Update Firmware
                        </ThemedText>

                        <View style={styles.buttonRow}>
                            <AppButton
                                title="Select Firmware Package (.zip)"
                                variant="primary"
                                style={styles.rowButton}
                                onPress={handleSelectFirmwarePackage}
                                disabled={!client}
                            />
                        </View>

                        <ThemedText type="overline" style={styles.sectionTitle}>
                            Device Actions
                        </ThemedText>

                        <View style={styles.buttonRow}>
                            <AppButton
                                title="Reset Device"
                                variant="secondary"
                                style={styles.rowButton}
                                onPress={handleReset}
                                disabled={!client}
                            />
                            <AppButton
                                title="Erase Slot 1"
                                variant="danger"
                                style={styles.rowButton}
                                onPress={handleEraseSlot}
                                disabled={!client}
                            />
                        </View>

                        {(blUpdaterRef.current || blRebooting) && renderBootloaderSection()}
                    </>
            </ScrollView>
        </SafeAreaView>
    );
}

const styles = StyleSheet.create({
    container: {
        flex: 1,
        padding: Spacing.lg,
    },
    header: {
        flexDirection: 'row',
        alignItems: 'center',
        marginBottom: Spacing.sm,
    },
    backButton: { flexDirection: 'row', alignItems: 'center' },
    scrollView: {
        flex: 1,
    },
    scrollContent: {
        paddingBottom: Spacing.lg,
        gap: Spacing.xs,
    },
    sectionTitle: {
        marginTop: Spacing.lg,
        marginBottom: Spacing.sm,
    },
    cardSpacing: {
        marginBottom: Spacing.sm,
        gap: 2,
    },
    slotTitle: {
        fontWeight: 'bold',
        fontSize: 16,
        marginBottom: Spacing.xs,
    },
    flagRow: {
        flexDirection: 'row',
        flexWrap: 'wrap',
        gap: Spacing.xs,
        marginTop: Spacing.xs,
    },
    slotButton: {
        marginTop: Spacing.sm,
    },
    packagePreview: {
        marginBottom: Spacing.lg,
    },
    firmwareTitle: {
        fontWeight: 'bold',
        fontSize: 14,
        marginBottom: Spacing.xs,
    },
    noImages: {
        fontStyle: 'italic',
    },
    infoRow: {
        flexDirection: 'row',
        alignItems: 'center',
        justifyContent: 'space-between',
        paddingVertical: Spacing.sm,
    },
    infoLabel: {
        fontSize: 16,
    },
    infoValue: {
        fontSize: 16,
        opacity: 0.5,
    },
    status: {
        marginTop: Spacing.sm,
        textAlign: 'center',
    },
    error: {
        marginTop: Spacing.sm,
        textAlign: 'center',
    },
    progressWrap: {
        marginTop: Spacing.sm,
    },
    buttonRow: {
        flexDirection: 'row',
        justifyContent: 'space-around',
        marginVertical: Spacing.sm,
        gap: Spacing.sm,
    },
    rowButton: {
        flex: 1,
    },
    link: {
        marginTop: Spacing.md,
        paddingVertical: Spacing.md,
        alignSelf: 'center',
    },
    updateCard: {
        alignItems: 'center',
        gap: Spacing.xs,
        marginBottom: Spacing.sm,
        marginTop: Spacing.xs,
    },
    updateCardError: {
        fontSize: 13,
        textAlign: 'center',
    },
    updateCardSuccess: {
        fontSize: 14,
        fontWeight: 'bold',
    },
    boardLabel: {
        opacity: 0.6,
    },
});
