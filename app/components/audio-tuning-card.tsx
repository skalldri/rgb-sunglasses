import { Link } from "expo-router";
import React from "react";
import { Pressable, StyleSheet, View, ViewStyle } from "react-native";

import { ThemedText } from "@/components/themed-text";
import { Card } from "@/components/ui/card";
import { IconSymbol } from "@/components/ui/icon-symbol";
import { UUID_AUDIO_CONFIG_SERVICE } from "@/constants/bluetooth";
import { Spacing } from "@/constants/theme";
import { useBluetooth } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";

/**
 * Controls-tab tile for the Audio Tuning screen.
 *
 * Does no reads or subscriptions of its own — discovery already populates every audio
 * characteristic, and none of them are notifiable, so there is nothing here that could go stale
 * in a way a tile should be reporting.
 *
 * Self-hides on firmware without the audio service, which is what keeps the Controls tab honest
 * on an older build rather than offering a screen that would open empty.
 */
export function AudioTuningCard({ style }: { style?: ViewStyle }) {
    const { selectedDevice } = useBluetooth();
    const c = useThemeColors();

    const audioChars = selectedDevice?.characteristicsByService?.[UUID_AUDIO_CONFIG_SERVICE];
    if (!audioChars || Object.keys(audioChars).length === 0) {
        return null;
    }

    return (
        <Link href="/(tabs)/device-state/audio" asChild>
            <Pressable accessibilityRole="button" accessibilityLabel="Audio tuning">
                <Card style={style}>
                    <View style={styles.row}>
                        <View style={styles.left}>
                            <IconSymbol name="waveform" size={20} color={c.primary} />
                            <View style={styles.labels}>
                                <ThemedText type="defaultSemiBold">Audio Tuning</ThemedText>
                                <ThemedText type="caption" style={{ color: c.textSecondary }}>
                                    How the glasses listen to the room
                                </ThemedText>
                            </View>
                        </View>
                        <IconSymbol name="chevron.right" size={18} color={c.textMuted} />
                    </View>
                </Card>
            </Pressable>
        </Link>
    );
}

const styles = StyleSheet.create({
    row: { flexDirection: "row", alignItems: "center", justifyContent: "space-between", gap: Spacing.md },
    left: { flexDirection: "row", alignItems: "center", gap: Spacing.md, flexShrink: 1 },
    labels: { gap: 1, flexShrink: 1 },
});
