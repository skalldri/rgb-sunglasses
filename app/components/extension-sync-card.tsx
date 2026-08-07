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
}

function statusLabel(status: ExtensionSyncStatus): string {
    switch (status) {
        case 'up-to-date':
            return 'Up to date';
        case 'outdated':
            return 'Update available';
        case 'missing':
            return 'Not installed';
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
}: ExtensionSyncCardProps) {
    const c = useThemeColors();

    if (state === 'idle') {
        return null;
    }

    if (state === 'checking') {
        return (
            <Card style={styles.card}>
                <ThemedText type="overline" style={styles.title}>
                    Extensions
                </ThemedText>
                <ActivityIndicator size="small" color={c.primary} />
                <ThemedText type="caption" style={styles.status}>
                    Checking extensions...
                </ThemedText>
            </Card>
        );
    }

    if (state === 'error') {
        return (
            <Card style={styles.card}>
                <ThemedText type="overline" style={styles.title}>
                    Extensions
                </ThemedText>
                <ThemedText type="caption" style={{ color: c.danger }}>
                    {/* Older firmware has no MCUmgr file management at all, so this
                        is an expected outcome, not necessarily a fault. */}
                    Extension check unavailable: {error}
                </ThemedText>
            </Card>
        );
    }

    if (entries.length === 0) {
        return (
            <Card style={styles.card}>
                <ThemedText type="overline" style={styles.title}>
                    Extensions
                </ThemedText>
                <ThemedText type="caption">This release ships no animation extensions.</ThemedText>
            </Card>
        );
    }

    const pending = entriesNeedingUpload(entries);

    return (
        <Card style={[styles.card, pending.length > 0 ? { borderColor: c.success } : null]}>
            <ThemedText type="overline" style={styles.title}>
                Extensions
            </ThemedText>

            {entries.map(entry => (
                <View key={entry.name} style={styles.row}>
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

            {unmanagedCount > 0 && (
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
                        progress={progress.bytesTotal > 0 ? progress.bytesSent / progress.bytesTotal : 0}
                        label={`${Math.round(
                            progress.bytesTotal > 0
                                ? (progress.bytesSent / progress.bytesTotal) * 100
                                : 0
                        )}%`}
                        height={12}
                    />
                </View>
            )}

            {pending.length > 0 ? (
                <>
                    <ThemedText type="caption" style={styles.note}>
                        Extensions are read at boot, so a reboot is needed after syncing.
                    </ThemedText>
                    <View style={styles.buttonRow}>
                        <AppButton
                            title={isSyncing ? 'Syncing...' : 'Sync Extensions'}
                            variant="primary"
                            style={styles.rowButton}
                            onPress={onSync}
                            disabled={disabled || isSyncing}
                        />
                    </View>
                </>
            ) : (
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
