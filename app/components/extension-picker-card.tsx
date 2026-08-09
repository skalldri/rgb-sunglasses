import { ThemedText } from '@/components/themed-text';
import { Card } from '@/components/ui/card';
import { IconSymbol } from '@/components/ui/icon-symbol';
import { ProgressBar } from '@/components/ui/progress-bar';
import { useThemeColors } from '@/hooks/use-theme-color';
import { ExtensionPickerAction, ExtensionPickerItem } from '@/services/extension-management';
import { ExtensionSyncProgress } from '@/services/extension-sync';
import React from 'react';
import { Pressable, StyleSheet, View } from 'react-native';

interface ExtensionPickerCardProps {
    items: ExtensionPickerItem[];
    /** Selection state keyed by item name — owned by the caller. */
    selected: Record<string, boolean>;
    onToggle: (name: string) => void;
    /** Disables toggling (while extension work or the restart is running). */
    disabled: boolean;
    /** Name of the file currently being uploaded/removed, if any. */
    busyName: string | null;
    progress: ExtensionSyncProgress | null;
}

function actionLabel(action: ExtensionPickerAction): string {
    switch (action) {
        case 'install':
            return 'Install';
        case 'update':
            return 'Update';
        case 'repair':
            return 'Repair';
        case 'remove':
            return 'Remove';
    }
}

/**
 * The guided flow's per-extension picker (design §6): every extension action
 * the restart will take is a visible, individually toggleable row — updates of
 * extensions the user already has come preselected, installing something new
 * never is, and not-in-this-release files are highlighted with removal
 * suggested. There is deliberately no "select all": bulk install is the exact
 * behavior this picker replaces.
 */
export function ExtensionPickerCard({
    items,
    selected,
    onToggle,
    disabled,
    busyName,
    progress,
}: ExtensionPickerCardProps) {
    const c = useThemeColors();

    if (items.length === 0) {
        return (
            <Card testID="extension-picker-empty" style={styles.card}>
                <ThemedText type="overline" style={styles.title}>
                    Extensions
                </ThemedText>
                <ThemedText type="caption" style={{ color: c.success }}>
                    All extensions match this release — nothing to change.
                </ThemedText>
            </Card>
        );
    }

    return (
        <Card testID="extension-picker" style={styles.card}>
            <ThemedText type="overline" style={styles.title}>
                Extensions
            </ThemedText>
            <ThemedText type="caption">
                Chosen changes are applied right before the restart.
            </ThemedText>

            {items.map(item => {
                const isSelected = !!selected[item.name];
                const isBusy = busyName === item.name;
                return (
                    <Pressable
                        key={`${item.action}-${item.name}`}
                        testID={`extension-picker-item-${item.name}`}
                        accessibilityRole="checkbox"
                        accessibilityState={{ checked: isSelected, disabled }}
                        disabled={disabled}
                        onPress={() => onToggle(item.name)}
                        style={[
                            styles.row,
                            item.highlighted && [styles.highlighted, { borderColor: c.warning }],
                        ]}>
                        <IconSymbol
                            name={isSelected ? 'checkmark.square.fill' : 'square'}
                            size={22}
                            color={disabled ? c.textMuted : isSelected ? c.primary : c.text}
                        />
                        <View style={styles.rowText}>
                            <ThemedText numberOfLines={1}>
                                {actionLabel(item.action)} {item.name}
                            </ThemedText>
                            {item.highlighted && (
                                <ThemedText type="caption" style={{ color: c.warning }}>
                                    Not part of this release — removal suggested
                                </ThemedText>
                            )}
                            {isBusy && progress && (
                                <ProgressBar
                                    progress={
                                        progress.bytesTotal > 0
                                            ? progress.bytesSent / progress.bytesTotal
                                            : 0
                                    }
                                    height={10}
                                />
                            )}
                        </View>
                    </Pressable>
                );
            })}
        </Card>
    );
}

const styles = StyleSheet.create({
    card: { gap: 8 },
    title: { marginBottom: 2 },
    row: { flexDirection: 'row', alignItems: 'center', gap: 10 },
    rowText: { flexShrink: 1, flexGrow: 1, gap: 2 },
    highlighted: {
        borderWidth: StyleSheet.hairlineWidth,
        borderRadius: 8,
        padding: 8,
    },
});
