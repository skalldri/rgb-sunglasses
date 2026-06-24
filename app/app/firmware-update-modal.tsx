import { ThemedText } from '@/components/themed-text';
import { ThemedView } from '@/components/themed-view';
import { AppButton } from '@/components/ui/app-button';
import { Badge } from '@/components/ui/badge';
import { Card } from '@/components/ui/card';
import { ProgressBar } from '@/components/ui/progress-bar';
import { Spacing } from '@/constants/theme';
import { useBluetooth } from '@/context/bluetooth-context';
import { useThemeColors } from '@/hooks/use-theme-color';
import { useMcuMgrClient } from '@/hooks/use-mcumgr-client';
import {
    calculateOverallUploadProgress,
    findUploadedImageForIndex,
    FirmwarePackage,
    parseFirmwareImageIndex,
    parseFirmwarePackageFromBase64
} from '@/services/firmware-package';
import {
    compareVersions,
    extractBoardRevision,
    fetchLatestRelease,
    findAssetForBoard,
    GitHubAsset,
    parseVersionFromTag,
} from '@/services/github-releases';
import { formatBytes, formatHash, ImageSlot, SlotInfoResponse } from '@/services/mcumgr';
import * as DocumentPicker from 'expo-document-picker';
import * as LegacyFS from 'expo-file-system/legacy';
import { File } from 'expo-file-system/next';
import { Link } from 'expo-router';
import { useCallback, useEffect, useState } from 'react';
import { ActivityIndicator, ScrollView, StyleSheet, View } from 'react-native';

// ============================================================================
// Component
// ============================================================================

function flagTone(flag: string): 'success' | 'info' | 'neutral' {
    if (flag === 'Active' || flag === 'Confirmed' || flag === 'Bootable') return 'success';
    if (flag === 'Pending' || flag === 'Permanent') return 'info';
    return 'neutral';
}

export default function FirmwareUpdateModal() {
    const { selectedDevice, setSelectedDevice } = useBluetooth();
    const { client, isInitializing, error: initError } = useMcuMgrClient(selectedDevice?.device ?? null);
    const c = useThemeColors();
    const [imageState, setImageState] = useState<ImageSlot[]>([]);
    const [status, setStatus] = useState<string>('');
    const [error, setError] = useState<string>('');
    const [uploadProgress, setUploadProgress] = useState<number>(0);
    const [isUploading, setIsUploading] = useState(false);
    const [slotInfo, setSlotInfo] = useState<SlotInfoResponse | null>(null);
    const [firmwarePackage, setFirmwarePackage] = useState<FirmwarePackage | null>(null);
    const [currentUploadIndex, setCurrentUploadIndex] = useState<number>(0);

    // Board detection
    const [boardRevision, setBoardRevision] = useState<string | null>(null);
    const [boardDetectionError, setBoardDetectionError] = useState<string>('');

    // GitHub update check
    type UpdateCheckState = 'idle' | 'checking' | 'upToDate' | 'updateAvailable' | 'error';
    const [updateCheckState, setUpdateCheckState] = useState<UpdateCheckState>('idle');
    const [latestAsset, setLatestAsset] = useState<GitHubAsset | null>(null);
    const [latestVersion, setLatestVersion] = useState<string>('');
    const [updateCheckError, setUpdateCheckError] = useState<string>('');

    // Download
    const [isDownloading, setIsDownloading] = useState(false);
    const [downloadProgress, setDownloadProgress] = useState<number>(0);

    // Update context with client for cleanup on disconnect
    useEffect(() => {
        if (client && selectedDevice) {
            setSelectedDevice({
                ...selectedDevice,
                mcuMgrClient: client
            });
        }
    }, [client, selectedDevice?.mac]); // Only update when client or device MAC changes

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

    // Display initialization error if present
    useEffect(() => {
        if (initError) {
            setError(initError);
        }
    }, [initError]);

    // Check GitHub for the latest release once board revision is known
    useEffect(() => {
        if (!boardRevision || updateCheckState !== 'idle') return;

        async function checkForUpdates() {
            setUpdateCheckState('checking');
            try {
                const release = await fetchLatestRelease('skalldri', 'rgb-sunglasses');
                const asset = findAssetForBoard(release.assets, boardRevision!);
                if (!asset) {
                    throw new Error(`No firmware asset found for board: ${boardRevision}`);
                }

                const githubVersion = parseVersionFromTag(release.tag_name);
                const activeSlot = imageState.find(s => s.active && s.slot === 0);
                const deviceVersion = activeSlot?.version ?? '';
                const cmp = deviceVersion ? compareVersions(deviceVersion, githubVersion) : -1;

                setLatestAsset(asset);
                setLatestVersion(githubVersion);
                setUpdateCheckState(cmp < 0 ? 'updateAvailable' : 'upToDate');
            } catch (e: unknown) {
                setUpdateCheckError(e instanceof Error ? e.message : String(e));
                setUpdateCheckState('error');
            }
        }

        checkForUpdates();
    }, [boardRevision, imageState]);

    async function handleSelectFirmwarePackage() {
        try {
            setError('');
            const result = await DocumentPicker.getDocumentAsync({
                type: 'application/zip',
                copyToCacheDirectory: true,
            });

            if (result.canceled || !result.assets?.[0]) {
                return;
            }

            const file = result.assets[0];
            setStatus(`Loading: ${file.name}`);

            // Read file contents as base64 using new File API
            const fileRef = new File(file.uri);
            const base64Data = await fileRef.base64();

            // Parse zip file
            setStatus('Parsing firmware package...');
            const parsedPackage = await parseFirmwarePackageFromBase64(base64Data);
            setFirmwarePackage(parsedPackage);
            setStatus('');
        } catch (e: any) {
            setError(`Failed to load firmware package: ${e.message}`);
            setStatus('');
        }
    }

    async function handleStartUpdate() {
        if (!client || !firmwarePackage) return;

        setIsUploading(true);
        setUploadProgress(0);
        setError('');
        setCurrentUploadIndex(0);

        try {
            const totalImages = firmwarePackage.images.length;

            for (let i = 0; i < totalImages; i++) {
                const image = firmwarePackage.images[i];
                const imageIndex = parseFirmwareImageIndex(image.manifest.image_index);
                setCurrentUploadIndex(i);

                setStatus(`Uploading ${image.manifest.file} (${i + 1}/${totalImages})...`);

                await client.uploadImage(
                    image.data,
                    imageIndex,
                    (sent, total) => {
                        setUploadProgress(calculateOverallUploadProgress(i, totalImages, sent, total));
                    }
                );

                setStatus(`Marking image ${imageIndex} for test...`);

                // Get updated image state and mark for test
                const state = await client.getImageState();
                console.log(`Got image state after upload: ${JSON.stringify(state)}`)
                const uploadedImage = findUploadedImageForIndex(state.images, imageIndex);

                if (uploadedImage?.hash) {
                    console.log(`Setting image state!`);
                    await client.setImageState(uploadedImage.hash, true); // Confirm image, permanent install
                } else {
                    throw new Error(`Uploaded image not found in image state response`);
                }
            }

            setStatus('All firmware images uploaded successfully!');
            setFirmwarePackage(null); // Clear the package
            await refreshImageState();
        } catch (e: any) {
            setError(`Upload failed: ${e.message}`);
        } finally {
            setIsUploading(false);
        }
    }

    async function handleDownloadUpdate() {
        if (!latestAsset) return;

        setIsDownloading(true);
        setDownloadProgress(0);
        setError('');

        const destUri = (LegacyFS.cacheDirectory ?? '') + 'firmware-update.zip';

        try {
            const task = LegacyFS.createDownloadResumable(
                latestAsset.browser_download_url,
                destUri,
                {},
                ({ totalBytesWritten, totalBytesExpectedToWrite }: { totalBytesWritten: number; totalBytesExpectedToWrite: number }) => {
                    if (totalBytesExpectedToWrite > 0) {
                        setDownloadProgress(
                            Math.round((totalBytesWritten / totalBytesExpectedToWrite) * 100)
                        );
                    }
                }
            );

            const result = await task.downloadAsync();
            if (!result) {
                throw new Error('Download was cancelled');
            }

            setStatus('Parsing firmware package...');
            const fileRef = new File(result.uri);
            const base64Data = await fileRef.base64();
            const parsedPackage = await parseFirmwarePackageFromBase64(base64Data);
            setFirmwarePackage(parsedPackage);
            setStatus('');
        } catch (e: any) {
            setError(`Download failed: ${e.message}`);
        } finally {
            setIsDownloading(false);
            setDownloadProgress(0);
        }
    }

    function handleCancelPackage() {
        setFirmwarePackage(null);
        setStatus('');
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

    function formatBoardRevision(revision: string): string {
        if (revision === 'proto0') return 'Proto0';
        if (revision === 'dk') return 'DK';
        return revision;
    }

    function renderAutoUpdateSection() {
        if (isDownloading) {
            return (
                <Card style={styles.updateCard}>
                    <ThemedText type="overline">Downloading Update...</ThemedText>
                    <ProgressBar progress={downloadProgress / 100} label={`${downloadProgress}%`} height={12} />
                </Card>
            );
        }

        if (!boardRevision && !boardDetectionError) {
            return (
                <Card style={styles.updateCard}>
                    <ActivityIndicator size="small" color={c.primary} />
                    <ThemedText type="caption" style={styles.status}>Detecting board...</ThemedText>
                </Card>
            );
        }

        if (boardDetectionError && !boardRevision) {
            return (
                <Card style={styles.updateCard}>
                    <ThemedText style={[styles.updateCardError, { color: c.danger }]}>{boardDetectionError}</ThemedText>
                </Card>
            );
        }

        if (updateCheckState === 'checking') {
            return (
                <Card style={styles.updateCard}>
                    <ThemedText type="caption" style={styles.boardLabel}>
                        Board: {formatBoardRevision(boardRevision!)}
                    </ThemedText>
                    <ActivityIndicator size="small" color={c.primary} />
                    <ThemedText type="caption" style={styles.status}>Checking for updates...</ThemedText>
                </Card>
            );
        }

        if (updateCheckState === 'error') {
            return (
                <Card style={styles.updateCard}>
                    <ThemedText type="caption" style={styles.boardLabel}>
                        Board: {formatBoardRevision(boardRevision!)}
                    </ThemedText>
                    <ThemedText style={[styles.updateCardError, { color: c.danger }]}>
                        Update check failed: {updateCheckError}
                    </ThemedText>
                </Card>
            );
        }

        if (updateCheckState === 'upToDate') {
            return (
                <Card style={styles.updateCard}>
                    <ThemedText type="caption" style={styles.boardLabel}>
                        Board: {formatBoardRevision(boardRevision!)}
                    </ThemedText>
                    <ThemedText style={[styles.updateCardSuccess, { color: c.success }]}>
                        Up to date (v{latestVersion})
                    </ThemedText>
                </Card>
            );
        }

        if (updateCheckState === 'updateAvailable' && latestAsset) {
            const activeSlot = imageState.find(s => s.active && s.slot === 0);
            const deviceVersion = activeSlot?.version ?? 'Unknown';

            return (
                <Card style={[styles.updateCard, { borderColor: c.success }]}>
                    <ThemedText type="overline" style={styles.sectionTitle}>
                        Update Available
                    </ThemedText>
                    <ThemedText type="caption" style={styles.boardLabel}>
                        Board: {formatBoardRevision(boardRevision!)}
                    </ThemedText>
                    <ThemedText type="caption">Current: v{deviceVersion}</ThemedText>
                    <ThemedText type="caption">Latest: v{latestVersion}</ThemedText>
                    <View style={styles.buttonRow}>
                        <AppButton
                            title="Download Update"
                            variant="primary"
                            style={styles.rowButton}
                            onPress={handleDownloadUpdate}
                            disabled={!client || isUploading}
                        />
                    </View>
                </Card>
            );
        }

        return null;
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

    function renderFirmwarePackagePreview() {
        if (!firmwarePackage) return null;

        return (
            <View style={styles.packagePreview}>
                <ThemedText type="overline" style={styles.sectionTitle}>
                    Firmware Package: {firmwarePackage.manifest.name}
                </ThemedText>

                {firmwarePackage.images.map((image, idx) => (
                    <Card key={idx} style={styles.cardSpacing}>
                        <ThemedText style={[styles.firmwareTitle, { color: c.warning }]}>
                            Image {image.manifest.image_index}: {image.manifest.file}
                        </ThemedText>
                        <ThemedText type="caption">
                            Type: {image.manifest.type}
                        </ThemedText>
                        <ThemedText type="caption">
                            Board: {image.manifest.board}
                        </ThemedText>
                        <ThemedText type="caption">
                            Size: {formatBytes(image.manifest.size)}
                        </ThemedText>
                        <ThemedText type="caption">
                            Version: {image.parsedHeader?.version || image.manifest.version_MCUBOOT || image.manifest.version || 'Unknown'}
                        </ThemedText>
                        <ThemedText type="caption">
                            Target Slots: {image.manifest.slot_index_primary} → {image.manifest.slot_index_secondary}
                        </ThemedText>
                    </Card>
                ))}

                <View style={styles.buttonRow}>
                    <AppButton
                        title="Cancel"
                        variant="secondary"
                        style={styles.rowButton}
                        onPress={handleCancelPackage}
                        disabled={isUploading}
                    />
                    <AppButton
                        title={isUploading ? `Uploading (${currentUploadIndex + 1}/${firmwarePackage.images.length})...` : 'Start Update'}
                        variant="primary"
                        style={styles.rowButton}
                        onPress={handleStartUpdate}
                        disabled={isUploading || !client}
                    />
                </View>
            </View>
        );
    }

    if (isInitializing) {
        return (
            <ThemedView style={styles.container}>
                <ActivityIndicator size="large" color={c.primary} />
                <ThemedText type="caption" style={styles.status}>{status || 'Initializing...'}</ThemedText>
            </ThemedView>
        );
    }

    return (
        <ThemedView style={styles.container}>
            {error ? (
                <ThemedText style={[styles.error, { color: c.danger }]}>{error}</ThemedText>
            ) : null}

            {status ? (
                <ThemedText type="caption" style={styles.status}>{status}</ThemedText>
            ) : null}

            {isUploading && (
                <View style={styles.progressWrap}>
                    <ProgressBar progress={uploadProgress / 100} label={`${uploadProgress}%`} height={12} />
                </View>
            )}

            <ScrollView style={styles.scrollView} contentContainerStyle={styles.scrollContent}>
                {/* Firmware Package Preview */}
                {renderFirmwarePackagePreview()}

                {/* Only show other sections if no package is loaded */}
                {!firmwarePackage && (
                    <>
                        {renderAutoUpdateSection()}

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
                                disabled={isUploading}
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
                                disabled={isUploading || !client}
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
                                disabled={isUploading || !client}
                            />
                            <AppButton
                                title="Erase Slot 1"
                                variant="danger"
                                style={styles.rowButton}
                                onPress={handleEraseSlot}
                                disabled={isUploading || !client}
                            />
                        </View>
                    </>
                )}
            </ScrollView>

            <Link href="../" style={styles.link}>
                <ThemedText type="link">Done</ThemedText>
            </Link>
        </ThemedView>
    );
}

const styles = StyleSheet.create({
    container: {
        flex: 1,
        padding: Spacing.lg,
    },
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
