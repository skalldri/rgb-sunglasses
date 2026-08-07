import { ExtensionSyncCard } from '@/components/extension-sync-card';
import { ThemedText } from '@/components/themed-text';
import { Card } from '@/components/ui/card';
import { IconSymbol } from '@/components/ui/icon-symbol';
import { Spacing } from '@/constants/theme';
import { useBluetooth } from '@/context/bluetooth-context';
import { useMcuMgrClientContext } from '@/context/mcumgr-client-context';
import { useExtensionSync } from '@/hooks/use-extension-sync';
import { useFirmwareRelease } from '@/context/firmware-update-context';
import { useThemeColors } from '@/hooks/use-theme-color';
import { useRouter } from 'expo-router';
import React from 'react';
import { Pressable, ScrollView, StyleSheet, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

/**
 * Animation-extension sync, on its own screen.
 *
 * Reached from the landing page and from the guided flow's success step. The release
 * lookup comes from the shared provider rather than a local hook — a per-screen lookup
 * fired a fresh unauthenticated GitHub request on every navigation.
 */
export default function ExtensionSyncScreen() {
    const c = useThemeColors();
    const router = useRouter();
    const { selectedDevice, reconnectingDevice } = useBluetooth();
    const { client } = useMcuMgrClientContext();
    const { releaseAssets, latestVersion, updateCheckState, updateCheckError } =
        useFirmwareRelease();
    const extensions = useExtensionSync(releaseAssets);

    const header = (
        <View style={styles.header}>
            <Pressable
                onPress={() => router.back()}
                accessibilityRole="button"
                accessibilityLabel="Back"
                hitSlop={8}
                style={styles.backButton}>
                <IconSymbol name="chevron.left" size={22} color={c.primary} />
                <ThemedText style={{ color: c.primary }}>Back</ThemedText>
            </Pressable>
        </View>
    );

    return (
        <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
            {header}
            <ScrollView contentContainerStyle={styles.content}>
                <ThemedText type="title" style={styles.title}>
                    Extensions
                </ThemedText>

                {selectedDevice == null ? (
                    <Card style={styles.card}>
                        <ThemedText type="caption">
                            {reconnectingDevice
                                ? `Reconnecting to ${reconnectingDevice.name}…`
                                : 'No device connected.'}
                        </ThemedText>
                    </Card>
                ) : (
                    <>
                        {updateCheckState === 'checking' && (
                            <ThemedText type="caption">Looking up the latest release…</ThemedText>
                        )}
                        {latestVersion ? (
                            <ThemedText type="caption">
                                Comparing against release v{latestVersion}
                            </ThemedText>
                        ) : null}

                        {/* Without this the card sits at 'idle' forever when the lookup
                            fails: releaseAssets stays empty so the sync plan never runs,
                            and the user gets an inert screen with no explanation. */}
                        {updateCheckState === 'error' && (
                            <ThemedText type="caption" style={{ color: c.danger }}>
                                Could not look up the latest release: {updateCheckError}. Extensions
                                are compared against a release, so syncing is unavailable until
                                this succeeds.
                            </ThemedText>
                        )}

                        <ExtensionSyncCard
                            state={extensions.state}
                            entries={extensions.entries}
                            unmanagedCount={extensions.unmanagedCount}
                            error={extensions.error}
                            isSyncing={extensions.isSyncing}
                            progress={extensions.progress}
                            onSync={extensions.sync}
                            disabled={!client}
                        />

                        <ThemedText type="caption">
                            Extensions are read when your sunglasses start up, so restart them
                            after syncing for the changes to take effect.
                        </ThemedText>
                    </>
                )}
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
});
