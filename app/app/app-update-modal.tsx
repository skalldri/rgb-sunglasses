import { Link } from 'expo-router';
import { useCallback, useEffect, useState } from 'react';
import { ActivityIndicator, Platform, ScrollView, StyleSheet, View } from 'react-native';
import * as WebBrowser from 'expo-web-browser';

import { ThemedText } from '@/components/themed-text';
import { ThemedView } from '@/components/themed-view';
import { AppButton } from '@/components/ui/app-button';
import { Card } from '@/components/ui/card';
import { ProgressBar } from '@/components/ui/progress-bar';
import { Spacing } from '@/constants/theme';
import { useThemeColors } from '@/hooks/use-theme-color';
import {
    AppUpdateInfo,
    checkForAppUpdate,
    downloadApk,
    getCurrentAppVersion,
    installApk,
} from '@/services/app-update';

type CheckState = 'checking' | 'upToDate' | 'updateAvailable' | 'error';

export default function AppUpdateModal() {
    const c = useThemeColors();
    const currentVersion = getCurrentAppVersion();
    const isAndroid = Platform.OS === 'android';

    const [checkState, setCheckState] = useState<CheckState>('checking');
    const [updateInfo, setUpdateInfo] = useState<AppUpdateInfo | null>(null);
    const [error, setError] = useState<string>('');

    const [isDownloading, setIsDownloading] = useState(false);
    const [downloadProgress, setDownloadProgress] = useState(0);
    const [apkUri, setApkUri] = useState<string | null>(null);
    const [isInstalling, setIsInstalling] = useState(false);

    const runCheck = useCallback(async () => {
        setCheckState('checking');
        setError('');
        setApkUri(null);
        setDownloadProgress(0);
        try {
            const info = await checkForAppUpdate();
            if (info) {
                setUpdateInfo(info);
                setCheckState('updateAvailable');
            } else {
                setUpdateInfo(null);
                setCheckState('upToDate');
            }
        } catch (e: any) {
            setError(e?.message ?? 'Unknown error');
            setCheckState('error');
        }
    }, []);

    useEffect(() => {
        runCheck();
    }, [runCheck]);

    async function handleDownload() {
        if (!updateInfo?.apkAsset) return;
        setIsDownloading(true);
        setDownloadProgress(0);
        setError('');
        try {
            const uri = await downloadApk(updateInfo.apkAsset, setDownloadProgress);
            setApkUri(uri);
        } catch (e: any) {
            setError(`Download failed: ${e?.message ?? 'Unknown error'}`);
        } finally {
            setIsDownloading(false);
        }
    }

    async function handleInstall() {
        if (!apkUri) return;
        setIsInstalling(true);
        setError('');
        try {
            await installApk(apkUri);
            // The system installer takes over from here; nothing more to do in-app.
        } catch (e: any) {
            setError(`Install failed: ${e?.message ?? 'Unknown error'}`);
        } finally {
            setIsInstalling(false);
        }
    }

    async function handleViewRelease() {
        if (updateInfo?.htmlUrl) {
            await WebBrowser.openBrowserAsync(updateInfo.htmlUrl);
        }
    }

    function renderContent() {
        if (checkState === 'checking') {
            return (
                <Card style={styles.card}>
                    <ActivityIndicator size="small" color={c.primary} />
                    <ThemedText type="caption" style={styles.centered}>Checking for updates...</ThemedText>
                </Card>
            );
        }

        if (checkState === 'error') {
            return (
                <Card style={styles.card}>
                    <ThemedText style={{ color: c.danger }}>Update check failed: {error}</ThemedText>
                </Card>
            );
        }

        if (checkState === 'upToDate') {
            return (
                <Card style={styles.card}>
                    <ThemedText style={{ color: c.success }}>
                        You&apos;re up to date (v{currentVersion}).
                    </ThemedText>
                </Card>
            );
        }

        // updateAvailable
        if (!updateInfo) return null;

        return (
            <Card style={[styles.card, { borderColor: c.success }]}>
                <ThemedText type="overline" style={styles.sectionTitle}>Update Available</ThemedText>
                <ThemedText type="caption">Current: v{currentVersion}</ThemedText>
                <ThemedText type="caption">Latest: v{updateInfo.version}</ThemedText>

                {updateInfo.release.body ? (
                    <ThemedText type="caption" style={styles.notes} numberOfLines={12}>
                        {updateInfo.release.body}
                    </ThemedText>
                ) : null}

                {error ? <ThemedText style={[styles.error, { color: c.danger }]}>{error}</ThemedText> : null}

                {isDownloading && (
                    <View style={styles.progressWrap}>
                        <ProgressBar progress={downloadProgress / 100} label={`${downloadProgress}%`} height={12} />
                    </View>
                )}

                {isAndroid ? (
                    <View style={styles.buttonRow}>
                        {apkUri ? (
                            <AppButton
                                title={isInstalling ? 'Opening installer...' : 'Install'}
                                variant="primary"
                                style={styles.rowButton}
                                onPress={handleInstall}
                                disabled={isInstalling}
                            />
                        ) : (
                            <AppButton
                                title={isDownloading ? `Downloading (${downloadProgress}%)...` : 'Download'}
                                variant="primary"
                                style={styles.rowButton}
                                onPress={handleDownload}
                                disabled={isDownloading || !updateInfo.apkAsset}
                            />
                        )}
                    </View>
                ) : (
                    <View style={styles.buttonRow}>
                        <AppButton
                            title="View Release"
                            variant="primary"
                            style={styles.rowButton}
                            onPress={handleViewRelease}
                            disabled={!updateInfo.htmlUrl}
                        />
                    </View>
                )}

                {isAndroid && !updateInfo.apkAsset && (
                    <ThemedText type="caption" style={{ color: c.danger }}>
                        This release has no APK asset to install.
                    </ThemedText>
                )}
            </Card>
        );
    }

    return (
        <ThemedView style={styles.container}>
            <ScrollView style={styles.scrollView} contentContainerStyle={styles.scrollContent}>
                {renderContent()}

                <View style={styles.buttonRow}>
                    <AppButton
                        title="Check Again"
                        variant="secondary"
                        style={styles.rowButton}
                        onPress={runCheck}
                        disabled={checkState === 'checking' || isDownloading || isInstalling}
                    />
                </View>
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
    card: {
        gap: Spacing.xs,
        marginTop: Spacing.md,
    },
    sectionTitle: {
        marginBottom: Spacing.xs,
    },
    centered: {
        textAlign: 'center',
    },
    notes: {
        marginTop: Spacing.sm,
    },
    error: {
        marginTop: Spacing.sm,
    },
    progressWrap: {
        marginTop: Spacing.sm,
    },
    buttonRow: {
        flexDirection: 'row',
        gap: Spacing.sm,
        marginTop: Spacing.md,
    },
    rowButton: {
        flex: 1,
    },
    link: {
        marginTop: Spacing.md,
        paddingVertical: Spacing.sm,
        alignSelf: 'center',
    },
});
