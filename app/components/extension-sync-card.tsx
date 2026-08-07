import { ThemedText } from '@/components/themed-text';
import { AppButton } from '@/components/ui/app-button';
import { Card } from '@/components/ui/card';
import { ProgressBar } from '@/components/ui/progress-bar';
import { useThemeColors } from '@/hooks/use-theme-color';
import {
    ExtensionSyncEntry,
    ExtensionSyncProgress,
    ExtensionSyncStatus,
    entriesNeedingUpload,
} from '@/services/extension-sync';
import React from 'react';
import { ActivityIndicator, StyleSheet, View } from 'react-native';

export type ExtensionCheckState = 'idle' | 'checking' | 'ready' | 'error';

interface ExtensionSyncCardProps {
    state: ExtensionCheckState;
    entries: ExtensionSyncEntry[];
    /** How many device extensions this release doesn't account for. */
    unmanagedCount: number;
    error: string;
    isSyncing: boolean;
    progress: ExtensionSyncProgress | null;
    onSync: () => void;
    disabled: boolean;
    /**
     * Hide the card's own Sync button.
     *
     * Used by the guided flow, which embeds this card on its "Ready to restart" step
     * and drives the sync from its own "Restart and Install" button — extensions are
     * written before the activating restart, so the two actions are one decision and
     * two buttons would invite the user to do half of it.
     */
    showSyncButton?: boolean;
}

function statusLabel(status: ExtensionSyncStatus): string {
    switch (status) {
        case 'up-to-date':
            return 'Up to date';
        case 'outdated':
            return 'Update available';
        case 'missing':
            return 'Not installed';
        case 'unreadable':
            return 'Needs repair';
    }
}

/**
 * Animation extensions shipped with the firmware release, and whether the
 * device's copies match.
 *
 * Extensions are ordinary files on the board's FAT disk, read at boot — so a
 * sync only takes effect after a reboot, and the copy below says so rather than
 * leaving the user to wonder why nothing changed.
 */
export function ExtensionSyncCard({
    state,
    entries,
    unmanagedCount,
    error,
    isSyncing,
    progress,
    onSync,
    disabled,
    showSyncButton = true,
}: ExtensionSyncCardProps) {
    const c = useThemeColors();

    if (state === 'idle') {
        return null;
    }

    const pending = entriesNeedingUpload(entries);
    // An error does not discard a plan we already have. A sync that fails
    // part-way through is exactly the case handleStartUpdate tells the user to
    // retry from this card, so the list and the retry button have to survive it -
    // otherwise they are left with a half-synced directory and no way back.
    const hasPlan = state === 'ready' || entries.length > 0;
    const showOwnSyncButton = showSyncButton && hasPlan && pending.length > 0;

    // One wrapper for every state, so the heading and card styling can't drift
    // apart between them.
    return (
        <Card
            testID={`extension-sync-${state}`}
            style={[
                styles.card,
                state !== 'error' && pending.length > 0 ? { borderColor: c.success } : null,
            ]}
        >
            <ThemedText type="overline" style={styles.title}>
                Extensions
            </ThemedText>

            {state === 'checking' && (
                <>
                    <ActivityIndicator size="small" color={c.primary} />
                    <ThemedText type="caption" style={styles.status}>
                        Checking extensions...
                    </ThemedText>
                </>
            )}

            {state === 'error' && (
                <ThemedText type="caption" style={{ color: c.danger }}>
                    {/* Older firmware has no MCUmgr file management at all, so this
                        is an expected outcome, not necessarily a fault. */}
                    {hasPlan ? 'Extension sync failed' : 'Extension check unavailable'}: {error}
                </ThemedText>
            )}

            {hasPlan &&
                entries.map(entry => (
                    <View
                        key={entry.name}
                        testID={`extension-sync-entry-${entry.name}`}
                        style={styles.row}>
                        <ThemedText type="caption" style={styles.rowName} numberOfLines={1}>
                            {entry.name}
                        </ThemedText>
                        <ThemedText
                            type="caption"
                            style={{
                                color: entry.status === 'up-to-date' ? c.success : c.primary,
                            }}
                        >
                            {statusLabel(entry.status)}
                        </ThemedText>
                    </View>
                ))}

            {state === 'ready' && entries.length === 0 && (
                <ThemedText type="caption">
                    This release ships no animation extensions.
                </ThemedText>
            )}

            {hasPlan && unmanagedCount > 0 && (
                <ThemedText type="caption" style={styles.note}>
                    {/* Deliberately a count, not a list: MCUmgr can neither list a
                        directory nor delete, and the device reports extension
                        display names rather than file names, so naming them here
                        would be guesswork. */}
                    {unmanagedCount === 1
                        ? '1 extension on this device is not part of this release.'
                        : `${unmanagedCount} extensions on this device are not part of this release.`}{' '}
                    It will keep working, but can only be removed over USB.
                </ThemedText>
            )}

            {isSyncing && progress && (
                <View style={styles.progressWrap}>
                    <ThemedText type="caption" style={styles.status}>
                        Uploading {progress.entry.name} ({progress.index + 1}/{progress.total})
                    </ThemedText>
                    <ProgressBar
                        progress={
                            progress.bytesTotal > 0 ? progress.bytesSent / progress.bytesTotal : 0
                        }
                        label={`${Math.round(
                            progress.bytesTotal > 0
                                ? (progress.bytesSent / progress.bytesTotal) * 100
                                : 0
                        )}%`}
                        height={12}
                    />
                </View>
            )}

            {showOwnSyncButton && (
                <>
                    <ThemedText type="caption" style={styles.note}>
                        Extensions are read at boot, so a reboot is needed after syncing.
                    </ThemedText>
                    <View style={styles.buttonRow}>
                        <AppButton
                            testID="extension-sync-button"
                            title={
                                isSyncing
                                    ? 'Syncing...'
                                    : state === 'error'
                                      ? 'Retry Sync'
                                      : 'Sync Extensions'
                            }
                            variant="primary"
                            style={styles.rowButton}
                            onPress={onSync}
                            disabled={disabled || isSyncing}
                        />
                    </View>
                </>
            )}

            {showSyncButton && state === 'ready' && entries.length > 0 && pending.length === 0 && (
                <ThemedText type="caption" style={{ color: c.success }}>
                    All extensions match this release.
                </ThemedText>
            )}
        </Card>
    );
}

const styles = StyleSheet.create({
    card: { marginBottom: 12, gap: 4 },
    title: { marginBottom: 4 },
    status: { marginTop: 4 },
    row: { flexDirection: 'row', justifyContent: 'space-between', gap: 8 },
    rowName: { flexShrink: 1 },
    note: { marginTop: 6, opacity: 0.8 },
    progressWrap: { marginTop: 8, gap: 4 },
    buttonRow: { flexDirection: 'row', marginTop: 8 },
    rowButton: { flex: 1 },
});
