import { ThemedText } from '@/components/themed-text';
import { AppButton } from '@/components/ui/app-button';
import { Card } from '@/components/ui/card';
import { IconSymbol } from '@/components/ui/icon-symbol';
import { ProgressBar } from '@/components/ui/progress-bar';
import { Spacing } from '@/constants/theme';
import { useBluetooth } from '@/context/bluetooth-context';
import { useFirmwareRelease } from '@/context/firmware-update-context';
import { useMcuMgrClientContext } from '@/context/mcumgr-client-context';
import { useExtensionManagement } from '@/hooks/use-extension-management';
import { useThemeColors } from '@/hooks/use-theme-color';
import { deviceFileState, DeviceFileState, ReleasedExtensionRow, UnmanagedExtensionRow } from '@/services/extension-management';
import { ExtensionSyncStatus } from '@/services/extension-sync';
import { useRouter } from 'expo-router';
import React from 'react';
import { ActivityIndicator, Alert, Pressable, ScrollView, StyleSheet, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

function releaseStatusLabel(status: ExtensionSyncStatus): string {
    switch (status) {
        case 'up-to-date':
            return 'Installed · up to date';
        case 'outdated':
            return 'Update available';
        case 'missing':
            return 'Not installed';
        case 'unreadable':
            return 'Needs repair';
    }
}

function releaseActionLabel(status: ExtensionSyncStatus): string | null {
    switch (status) {
        case 'outdated':
            return 'Update';
        case 'missing':
            return 'Install';
        case 'unreadable':
            return 'Repair';
        case 'up-to-date':
            return null;
    }
}

function deviceStateCaption(state: DeviceFileState): string | null {
    switch (state) {
        case 'pending-restart':
            return 'Takes effect after restart';
        case 'removed':
            return 'Removed — restart to free the slot';
        case 'faulted':
            return 'Faulted — restart to retry';
        case 'installed':
            return null;
    }
}

/** House promisified destructive confirm. */
function confirmRemove(name: string): Promise<boolean> {
    return new Promise<boolean>(resolve => {
        Alert.alert(
            `Remove ${name}?`,
            'The file is deleted from your sunglasses immediately. If its animation is ' +
                'running, the display switches away first. Its slot stays reserved until ' +
                'the next restart.',
            [
                { text: 'Cancel', style: 'cancel', onPress: () => resolve(false) },
                { text: 'Remove', style: 'destructive', onPress: () => resolve(true) },
            ]
        );
    });
}

/**
 * The extension-management surface (design: fw/docs/extension-management.md §6).
 *
 * One screen, always reachable from the firmware-update landing page and the
 * guided flow: what this release ships (install/update/remove per row), and
 * what the device carries that the release doesn't — where every provisioned
 * board's stale `hello.llext` finally becomes visible and removable over BLE.
 *
 * There is deliberately NO "sync everything" button: install is always a
 * per-extension user choice, which is also what makes an uninstall stick —
 * nothing ever re-installs a removed extension behind the user's back.
 */
export default function ExtensionManagementScreen() {
    const c = useThemeColors();
    const router = useRouter();
    const { selectedDevice, reconnectingDevice } = useBluetooth();
    const { client } = useMcuMgrClientContext();
    const { releaseAssets, latestVersion, updateCheckState, updateCheckError } =
        useFirmwareRelease();
    // "Known" = the lookup succeeded. On error/in-flight the plan must treat
    // the release as unknown, never as empty — see useExtensionManagement.
    const releaseKnown = updateCheckState === 'upToDate' || updateCheckState === 'updateAvailable';
    const mgmt = useExtensionManagement(releaseAssets, releaseKnown);

    async function handleRemove(name: string) {
        if (await confirmRemove(name)) {
            await mgmt.removeOne(name);
        }
    }

    function renderProgress(name: string) {
        if (mgmt.busyName !== name || !mgmt.progress) return null;
        const { bytesSent, bytesTotal } = mgmt.progress;
        return (
            <ProgressBar
                progress={bytesTotal > 0 ? bytesSent / bytesTotal : 0}
                label={`${Math.round(bytesTotal > 0 ? (bytesSent / bytesTotal) * 100 : 0)}%`}
                height={10}
            />
        );
    }

    function renderReleasedRow(row: ReleasedExtensionRow) {
        const { entry, device } = row;
        const action = releaseActionLabel(entry.status);
        const stateCaption = device ? deviceStateCaption(deviceFileState(device)) : null;
        const busy = mgmt.busyName !== null;
        // Remove applies to the on-device copy, so it needs the device to have
        // one AND the firmware to support FILE_MGMT at all.
        const removable = mgmt.plan.listAvailable && device?.onDisk === true;

        return (
            <View key={entry.name} testID={`ext-mgmt-released-${entry.name}`} style={styles.row}>
                <View style={styles.rowText}>
                    <ThemedText numberOfLines={1}>{entry.name}</ThemedText>
                    <ThemedText
                        type="caption"
                        style={{ color: entry.status === 'up-to-date' ? c.success : c.primary }}>
                        {releaseStatusLabel(entry.status)}
                    </ThemedText>
                    {stateCaption && (
                        <ThemedText type="caption" style={styles.stateCaption}>
                            {stateCaption}
                        </ThemedText>
                    )}
                    {renderProgress(entry.name)}
                </View>
                <View style={styles.rowActions}>
                    {action && (
                        <AppButton
                            testID={`ext-mgmt-install-${entry.name}`}
                            title={mgmt.busyName === entry.name ? '…' : action}
                            variant="primary"
                            onPress={() => mgmt.installOne(entry)}
                            disabled={busy || !client}
                        />
                    )}
                    {removable && (
                        <AppButton
                            testID={`ext-mgmt-remove-${entry.name}`}
                            title="Remove"
                            variant="secondary"
                            onPress={() => handleRemove(entry.name)}
                            disabled={busy || !client}
                        />
                    )}
                </View>
            </View>
        );
    }

    function renderUnmanagedRow(row: UnmanagedExtensionRow) {
        const { device, state } = row;
        const caption = deviceStateCaption(state);
        const busy = mgmt.busyName !== null;

        return (
            <View
                key={device.name}
                testID={`ext-mgmt-unmanaged-${device.name}`}
                style={[styles.row, styles.unmanagedRow, { borderColor: c.warning }]}>
                <View style={styles.rowText}>
                    <ThemedText numberOfLines={1}>{device.name}</ThemedText>
                    {device.displayName && (
                        <ThemedText type="caption">{device.displayName}</ThemedText>
                    )}
                    <ThemedText type="caption" style={{ color: c.warning }}>
                        Not part of this release
                    </ThemedText>
                    {caption && (
                        <ThemedText type="caption" style={styles.stateCaption}>
                            {caption}
                        </ThemedText>
                    )}
                </View>
                <View style={styles.rowActions}>
                    {device.onDisk && (
                        <AppButton
                            testID={`ext-mgmt-remove-${device.name}`}
                            title={mgmt.busyName === device.name ? '…' : 'Remove'}
                            variant="secondary"
                            onPress={() => handleRemove(device.name)}
                            disabled={busy || !client}
                        />
                    )}
                </View>
            </View>
        );
    }

    function renderBody() {
        if (mgmt.state === 'checking' && mgmt.plan.released.length === 0) {
            return (
                <Card style={styles.card}>
                    <ActivityIndicator size="small" color={c.primary} />
                    <ThemedText type="caption" style={styles.centered}>
                        Checking extensions…
                    </ThemedText>
                </Card>
            );
        }

        return (
            <>
                {mgmt.state === 'error' && (
                    <Card style={[styles.card, { borderColor: c.danger }]}>
                        <ThemedText type="caption" style={{ color: c.danger }}>
                            {mgmt.error}
                        </ThemedText>
                    </Card>
                )}

                <Card testID="ext-mgmt-released" style={styles.card}>
                    <ThemedText type="overline">From this release</ThemedText>
                    {mgmt.plan.released.length === 0 ? (
                        <ThemedText type="caption">
                            {/* "Ships no extensions" is a factual claim — only make it
                                when the check actually SUCCEEDED. A failed check (old
                                firmware, transport error) must say so, not assert an
                                empty release. */}
                            {!releaseKnown
                                ? 'Release lookup failed, so release extensions cannot be shown.'
                                : mgmt.state === 'error'
                                  ? 'Extension check failed — release extensions cannot be shown.'
                                  : 'This release ships no animation extensions.'}
                        </ThemedText>
                    ) : (
                        mgmt.plan.released.map(renderReleasedRow)
                    )}
                </Card>

                {mgmt.plan.listAvailable ? (
                    <Card testID="ext-mgmt-unmanaged" style={styles.card}>
                        <ThemedText type="overline">Not in this release</ThemedText>
                        {!mgmt.plan.releaseKnown ? (
                            <ThemedText type="caption">
                                Unavailable until the release lookup succeeds — files on your
                                sunglasses cannot be compared against an unknown release.
                            </ThemedText>
                        ) : mgmt.plan.unmanaged.length === 0 ? (
                            <ThemedText type="caption">
                                Everything on your sunglasses comes from this release.
                            </ThemedText>
                        ) : (
                            mgmt.plan.unmanaged.map(renderUnmanagedRow)
                        )}
                    </Card>
                ) : (
                    mgmt.state === 'ready' && (
                        <ThemedText type="caption">
                            This firmware cannot list or remove extension files over Bluetooth —
                            update it to manage extensions here.
                        </ThemedText>
                    )
                )}

                {mgmt.mutated && (
                    <Card style={[styles.card, { borderColor: c.success }]}>
                        <ThemedText type="caption">
                            Extension changes are read when your sunglasses start up — restart
                            them for the changes to take effect.
                        </ThemedText>
                        <AppButton
                            testID="ext-mgmt-restart"
                            title="Restart glasses now"
                            variant="primary"
                            style={styles.restartButton}
                            onPress={mgmt.reboot}
                            disabled={mgmt.busyName !== null || !client}
                        />
                    </Card>
                )}
            </>
        );
    }

    return (
        <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
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
                        {/* Without this the screen sits inert when the lookup fails: the
                            release section stays empty with no explanation, even though
                            the device-side list below still works. */}
                        {updateCheckState === 'error' && (
                            <ThemedText type="caption" style={{ color: c.danger }}>
                                Could not look up the latest release: {updateCheckError}.
                            </ThemedText>
                        )}
                        {renderBody()}
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
    card: { gap: Spacing.sm },
    centered: { textAlign: 'center' },
    row: {
        flexDirection: 'row',
        alignItems: 'center',
        justifyContent: 'space-between',
        gap: Spacing.sm,
    },
    rowText: { flexShrink: 1, flexGrow: 1, gap: 2 },
    rowActions: { flexDirection: 'row', gap: Spacing.xs },
    unmanagedRow: {
        borderWidth: StyleSheet.hairlineWidth,
        borderRadius: 8,
        padding: Spacing.sm,
    },
    stateCaption: { opacity: 0.8 },
    restartButton: { marginTop: Spacing.xs },
});
