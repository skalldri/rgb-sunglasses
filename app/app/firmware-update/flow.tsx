import { ThemedText } from '@/components/themed-text';
import { AppButton } from '@/components/ui/app-button';
import { Card } from '@/components/ui/card';
import { IconSymbol } from '@/components/ui/icon-symbol';
import { ProgressBar } from '@/components/ui/progress-bar';
import { Spacing } from '@/constants/theme';
import { useFirmwareUpdateFlow, type FlowStep } from '@/hooks/use-firmware-update-flow';
import { useThemeColors } from '@/hooks/use-theme-color';
import { FirmwarePackage } from '@/services/firmware-package';
import { loadPackage, type FirmwareSource } from '@/services/firmware-source';
import { formatBytes } from '@/services/mcumgr';
import { Link, useLocalSearchParams, useRouter } from 'expo-router';
import React, { useEffect, useState } from 'react';
import { ActivityIndicator, Pressable, ScrollView, StyleSheet, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

/** Human-readable heading per step. */
const STEP_TITLE: Record<FlowStep, string> = {
    loadingSource: 'Preparing update',
    ready: 'Ready to install',
    uploading: 'Uploading firmware',
    staging: 'Preparing images',
    staged: 'Ready to restart',
    rebooting: 'Restarting device',
    reconnecting: 'Waiting for device',
    verifying: 'Verifying installation',
    success: 'Update complete',
    failed: 'Update failed',
};

/**
 * The guided end-to-end firmware update.
 *
 * Deliberately owns the whole journey on one screen: picking a source, uploading,
 * restarting, waiting for the device to come back, verifying and confirming. Doing it
 * across several routes would risk unmounting mid-reboot and losing the reference
 * hashes that verification depends on.
 *
 * This screen must NOT mount `useDisconnectRedirect` — the device disappearing is an
 * expected part of the flow, not a reason to bounce the user to the Connect tab.
 */
export default function FirmwareUpdateFlow() {
    const c = useThemeColors();
    const router = useRouter();
    const params = useLocalSearchParams();

    const [pkg, setPkg] = useState<FirmwarePackage | null>(null);
    const [downloadPercent, setDownloadPercent] = useState(0);
    const [loadError, setLoadError] = useState('');

    const flow = useFirmwareUpdateFlow(pkg);

    // Resolve the source descriptor into a package. Params are strings only, so the
    // landing page hands over a URI/URL and the package is re-derived here.
    useEffect(() => {
        const kind = params.source as string | undefined;
        if (!kind) return;

        const source: FirmwareSource | null =
            kind === 'file'
                ? { kind: 'file', uri: params.uri as string, name: params.name as string }
                : kind === 'release'
                  ? {
                        kind: 'release',
                        url: params.url as string,
                        version: params.version as string,
                    }
                  : null;

        if (!source) {
            setLoadError(`Unknown update source: ${kind}`);
            return;
        }

        let cancelled = false;
        loadPackage(source, setDownloadPercent)
            .then(parsed => {
                if (!cancelled) setPkg(parsed);
            })
            .catch((e: unknown) => {
                if (!cancelled) setLoadError(e instanceof Error ? e.message : String(e));
            });

        return () => {
            cancelled = true;
        };
        // Params are stable for the life of this screen; re-running would re-download.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    const header = (
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
    );

    /**
     * Per-image cards. Border and text go from warning-yellow to success-green once the
     * image has been staged, and again carry green through verification — this is the
     * visible signal that the images are actually on the device.
     */
    function renderImageCards() {
        return flow.images.map(img => {
            const done = img.verified || img.staged;
            const tone = done ? c.success : c.warning;
            return (
                <Card key={`${img.imageIndex}-${img.file}`} style={[styles.card, { borderColor: tone }]}>
                    <ThemedText style={{ color: tone }}>
                        Image {img.imageIndex}: {img.file}
                    </ThemedText>
                    <ThemedText type="caption">Version: {img.version}</ThemedText>
                    <ThemedText type="caption">
                        {img.verified
                            ? 'Verified — running on the device'
                            : img.staged
                              ? 'Staged, waiting for restart'
                              : img.uploaded
                                ? 'Uploaded'
                                : 'Not uploaded yet'}
                    </ThemedText>
                </Card>
            );
        });
    }

    function renderBody() {
        if (loadError) {
            return (
                <Card style={styles.card}>
                    <ThemedText style={{ color: c.danger }}>{loadError}</ThemedText>
                    <View style={styles.buttonRow}>
                        <AppButton
                            title="Back"
                            variant="secondary"
                            style={styles.rowButton}
                            onPress={() => router.back()}
                        />
                    </View>
                </Card>
            );
        }

        switch (flow.step) {
            case 'loadingSource':
                return (
                    <Card style={styles.card}>
                        <ActivityIndicator size="small" color={c.primary} />
                        <ThemedText type="caption" style={styles.centered}>
                            {downloadPercent > 0
                                ? `Downloading… ${downloadPercent}%`
                                : 'Preparing…'}
                        </ThemedText>
                        {downloadPercent > 0 && (
                            <ProgressBar progress={downloadPercent / 100} height={12} />
                        )}
                    </Card>
                );

            case 'ready':
                return (
                    <>
                        {renderImageCards()}
                        <ThemedText type="caption">
                            {pkg?.images.length === 1
                                ? '1 image will be installed.'
                                : `${pkg?.images.length ?? 0} images will be installed.`}{' '}
                            Your sunglasses will restart at the end.
                        </ThemedText>
                        <View style={styles.buttonRow}>
                            <AppButton
                                title="Install"
                                variant="primary"
                                style={styles.rowButton}
                                onPress={flow.startUpload}
                            />
                        </View>
                    </>
                );

            case 'uploading':
            case 'staging':
                return (
                    <>
                        <Card style={styles.card}>
                            <ThemedText type="caption">
                                {flow.step === 'uploading'
                                    ? `Uploading image ${flow.currentImageIndex + 1} of ${flow.images.length}…`
                                    : 'Preparing images for install…'}
                            </ThemedText>
                            <ProgressBar
                                progress={flow.uploadProgress / 100}
                                label={`${flow.uploadProgress}%`}
                                height={12}
                            />
                            <ThemedText type="caption">
                                Keep this screen open and stay near your sunglasses.
                            </ThemedText>
                        </Card>
                        {renderImageCards()}
                    </>
                );

            case 'staged':
                return (
                    <>
                        {renderImageCards()}
                        <Card style={[styles.card, { borderColor: c.success }]}>
                            <ThemedText style={{ color: c.success }}>
                                Firmware uploaded and verified on the device.
                            </ThemedText>
                            <ThemedText type="caption">
                                Restart now to install it. Your sunglasses will be unavailable for
                                up to a minute, then reconnect on their own.
                            </ThemedText>
                            <View style={styles.buttonRow}>
                                <AppButton
                                    title="Restart and Install"
                                    variant="primary"
                                    style={styles.rowButton}
                                    onPress={flow.reboot}
                                />
                            </View>
                        </Card>
                    </>
                );

            // The device is legitimately gone during these two steps. Neutral progress,
            // never an error — this is the whole point of the redesign.
            case 'rebooting':
            case 'reconnecting':
                return (
                    <Card style={styles.card}>
                        <ActivityIndicator size="small" color={c.primary} />
                        <ThemedText type="caption" style={styles.centered}>
                            {flow.step === 'rebooting'
                                ? 'Restarting your sunglasses…'
                                : 'Waiting for your sunglasses to come back…'}
                        </ThemedText>
                        {flow.step === 'reconnecting' && (
                            <ThemedText type="caption" style={styles.centered}>
                                {Math.round(flow.reconnectElapsedMs / 1000)}s — installing the
                                update can take a minute.
                            </ThemedText>
                        )}
                        {flow.reconnectTakingLong && (
                            <ThemedText type="caption" style={[styles.centered, { color: c.warning }]}>
                                This is taking longer than usual. Make sure your sunglasses are
                                powered on and nearby — the app will keep trying.
                            </ThemedText>
                        )}
                    </Card>
                );

            case 'verifying':
                return (
                    <>
                        <Card style={styles.card}>
                            <ActivityIndicator size="small" color={c.primary} />
                            <ThemedText type="caption" style={styles.centered}>
                                Checking the update installed correctly…
                            </ThemedText>
                        </Card>
                        {renderImageCards()}
                    </>
                );

            case 'success':
                return (
                    <>
                        <Card style={[styles.card, { borderColor: c.success }]}>
                            <ThemedText style={{ color: c.success }}>
                                Your sunglasses are running the new firmware.
                            </ThemedText>
                        </Card>
                        {renderImageCards()}
                        <Card style={styles.card}>
                            <ThemedText type="caption">
                                Animation extensions are stored separately and may also need
                                updating.
                            </ThemedText>
                            <Link href="/firmware-update/extensions" asChild>
                                <AppButton
                                    title="Sync Extensions"
                                    variant="primary"
                                    style={styles.stackedButton}
                                />
                            </Link>
                            <AppButton
                                title="Done"
                                variant="secondary"
                                style={styles.stackedButton}
                                onPress={() => router.back()}
                            />
                        </Card>
                    </>
                );

            case 'failed':
                return (
                    <>
                        <Card style={[styles.card, { borderColor: c.danger }]}>
                            <ThemedText style={{ color: c.danger }}>{flow.error}</ThemedText>
                        </Card>
                        {renderImageCards()}
                        <Card style={styles.card}>
                            <AppButton
                                title="Try Again"
                                variant="primary"
                                style={styles.stackedButton}
                                onPress={flow.reset}
                            />
                            <Link href="/firmware-update/debug" asChild>
                                <AppButton
                                    title="Open FW Update Debug"
                                    variant="ghost"
                                    style={styles.stackedButton}
                                />
                            </Link>
                        </Card>
                    </>
                );
        }
    }

    return (
        <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
            {header}
            <ScrollView contentContainerStyle={styles.content}>
                <ThemedText type="title" style={styles.title}>
                    {STEP_TITLE[flow.step]}
                </ThemedText>
                {pkg && (
                    <ThemedText type="caption">
                        {pkg.manifest.name} ·{' '}
                        {formatBytes(pkg.images.reduce((n, i) => n + i.data.length, 0))}
                    </ThemedText>
                )}
                {renderBody()}
            </ScrollView>
        </SafeAreaView>
    );
}

const styles = StyleSheet.create({
    container: { flex: 1 },
    header: {
        flexDirection: 'row',
        alignItems: 'center',
        paddingHorizontal: Spacing.md,
        paddingVertical: Spacing.sm,
    },
    backButton: { flexDirection: 'row', alignItems: 'center' },
    content: { padding: Spacing.md, gap: Spacing.md },
    title: { marginBottom: Spacing.xs },
    card: { gap: 6 },
    centered: { textAlign: 'center' },
    buttonRow: { flexDirection: 'row', marginTop: Spacing.sm },
    rowButton: { flex: 1 },
    stackedButton: { marginTop: Spacing.sm },
});
