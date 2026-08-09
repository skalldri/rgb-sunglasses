import { ThemedText } from '@/components/themed-text';
import { AppButton } from '@/components/ui/app-button';
import { Card } from '@/components/ui/card';
import { IconSymbol } from '@/components/ui/icon-symbol';
import { Spacing } from '@/constants/theme';
import { useBluetooth } from '@/context/bluetooth-context';
import { useMcuMgrClientContext } from '@/context/mcumgr-client-context';
import { useFirmwareRelease } from '@/context/firmware-update-context';
import { useThemeColors } from '@/hooks/use-theme-color';
import * as DocumentPicker from 'expo-document-picker';
import { Link, useRouter } from 'expo-router';
import React, { useState } from 'react';
import { ActivityIndicator, Pressable, ScrollView, StyleSheet, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

function formatBoardRevision(revision: string): string {
    return revision === 'proto0' ? 'Proto0' : revision.toUpperCase();
}

/**
 * Firmware Update landing page — the everyday entry point.
 *
 * Deliberately small: install the latest release, install a local `.zip`, sync
 * extensions, or drop into the debug page. Everything diagnostic (slot tables, manual
 * reset/erase, the bootloader updater) lives on the debug page so a first-time user
 * is never asked to work out which button applies their update.
 */
export default function FirmwareUpdateLanding() {
    const c = useThemeColors();
    const router = useRouter();
    const { selectedDevice, reconnectingDevice } = useBluetooth();
    const { client, isInitializing, error: clientError } = useMcuMgrClientContext();
    const {
        boardRevision,
        boardDetectionError,
        updateCheckState,
        latestAsset,
        latestVersion,
        deviceVersion,
        updateCheckError,
    } = useFirmwareRelease();

    const [pickError, setPickError] = useState('');

    const isConnected = selectedDevice != null;

    async function handlePickFile() {
        try {
            setPickError('');
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
        } catch (e: unknown) {
            setPickError(e instanceof Error ? e.message : String(e));
        }
    }

    function handleInstallRelease() {
        if (!latestAsset) return;
        router.push({
            pathname: '/firmware-update/flow',
            params: {
                source: 'release',
                url: latestAsset.browser_download_url,
                version: latestVersion,
            },
        });
    }

    const header = (
        <View style={styles.header}>
            <Pressable
                testID="fw-update-landing-back"
                onPress={() => router.back()}
                accessibilityRole="button"
                accessibilityLabel="Close firmware update"
                hitSlop={8}
                style={styles.backButton}>
                <IconSymbol name="chevron.left" size={22} color={c.primary} />
                <ThemedText style={{ color: c.primary }}>Done</ThemedText>
            </Pressable>
        </View>
    );

    /**
     * Connection status. Rendered as neutral text, never danger red: a device that is
     * away is usually mid-reboot or briefly out of range, not broken. The old screen
     * showed "No device connected" in red here and never cleared it.
     */
    function renderConnectionStatus() {
        if (isConnected) return null;
        return (
            <Card style={styles.card}>
                <ThemedText type="caption">
                    {reconnectingDevice
                        ? `Reconnecting to ${reconnectingDevice.name}…`
                        : 'No device connected. Connect to your sunglasses to update them.'}
                </ThemedText>
            </Card>
        );
    }

    function renderReleaseCard() {
        if (!isConnected) return null;

        // A connected device with no client means SMP init failed (e.g. firmware with
        // no SMP characteristic). Without this the spinner below never resolves, since
        // board detection cannot run and leaves both of its outputs empty.
        if (!client && !isInitializing) {
            return (
                <Card style={styles.card}>
                    <ThemedText type="caption" style={{ color: c.danger }}>
                        {clientError || 'Firmware update is unavailable on this device.'}
                    </ThemedText>
                </Card>
            );
        }

        if (isInitializing || (!boardRevision && !boardDetectionError)) {
            return (
                <Card style={styles.card}>
                    <ActivityIndicator size="small" color={c.primary} />
                    <ThemedText type="caption" style={styles.centered}>
                        Checking your device…
                    </ThemedText>
                </Card>
            );
        }

        if (boardDetectionError && !boardRevision) {
            return (
                <Card style={styles.card}>
                    <ThemedText type="caption" style={{ color: c.danger }}>
                        {boardDetectionError}
                    </ThemedText>
                </Card>
            );
        }

        if (updateCheckState === 'checking') {
            return (
                <Card style={styles.card}>
                    <ActivityIndicator size="small" color={c.primary} />
                    <ThemedText type="caption" style={styles.centered}>
                        Checking for updates…
                    </ThemedText>
                </Card>
            );
        }

        if (updateCheckState === 'error') {
            return (
                <Card style={styles.card}>
                    <ThemedText type="caption" style={{ color: c.danger }}>
                        Update check failed: {updateCheckError}
                    </ThemedText>
                </Card>
            );
        }

        if (updateCheckState === 'upToDate') {
            return (
                <Card style={styles.card}>
                    <ThemedText type="overline">Firmware</ThemedText>
                    <ThemedText style={{ color: c.success }}>
                        Up to date (v{latestVersion})
                    </ThemedText>
                    <ThemedText type="caption">
                        Board: {formatBoardRevision(boardRevision!)}
                    </ThemedText>
                </Card>
            );
        }

        if (updateCheckState === 'updateAvailable' && latestAsset) {
            return (
                <Card style={[styles.card, { borderColor: c.success }]}>
                    <ThemedText type="overline">Update Available</ThemedText>
                    <ThemedText type="heading">v{latestVersion}</ThemedText>
                    <ThemedText type="caption">
                        Installed: v{deviceVersion || 'Unknown'} · Board:{' '}
                        {formatBoardRevision(boardRevision!)}
                    </ThemedText>
                    <View style={styles.buttonRow}>
                        <AppButton
                            testID="fw-update-install-release"
                            title="Install Update"
                            variant="primary"
                            style={styles.rowButton}
                            onPress={handleInstallRelease}
                        />
                    </View>
                </Card>
            );
        }

        return null;
    }

    return (
        <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
            {header}
            <ScrollView contentContainerStyle={styles.content}>
                <ThemedText type="title" style={styles.title}>
                    Firmware Update
                </ThemedText>

                {renderConnectionStatus()}
                {renderReleaseCard()}

                {pickError ? (
                    <ThemedText type="caption" style={{ color: c.danger }}>
                        {pickError}
                    </ThemedText>
                ) : null}

                <Card style={styles.card}>
                    <ThemedText type="overline">Other options</ThemedText>

                    <AppButton
                        testID="fw-update-pick-zip"
                        title="Install from a .zip file"
                        variant="secondary"
                        style={styles.stackedButton}
                        onPress={handlePickFile}
                        disabled={!isConnected}
                    />

                    <Link href="/firmware-update/extensions" asChild>
                        <AppButton
                            testID="fw-update-landing-sync-extensions"
                            title="Manage Extensions"
                            variant="secondary"
                            style={styles.stackedButton}
                            disabled={!isConnected}
                        />
                    </Link>

                    <Link href="/firmware-update/debug" asChild>
                        <AppButton
                            testID="fw-update-landing-debug"
                            title="FW Update Debug"
                            variant="ghost"
                            style={styles.stackedButton}
                        />
                    </Link>
                </Card>
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
