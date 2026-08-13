import { ThemedText } from "@/components/themed-text";
import { Badge } from "@/components/ui/badge";
import { Card } from "@/components/ui/card";
import { IconSymbol } from "@/components/ui/icon-symbol";
import { UUID_CAPTURE_COUNT, UUID_CAPTURE_STATE } from "@/constants/bluetooth";
import { Spacing } from "@/constants/theme";
import { useBluetooth } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { decodeUint32FromBase64 } from "@/services/ble-value-codec";
import { captureStateFromCode, captureStateLabel, captureStateTone } from "@/services/capture";
import { Link } from "expo-router";
import React from "react";
import { Pressable, StyleSheet, View, ViewStyle } from "react-native";

function decodeUint32OrNull(encoded: string | null | undefined): number | null {
    if (!encoded) return null;
    try {
        return decodeUint32FromBase64(encoded);
    } catch {
        return null;
    }
}

/**
 * Compact capture tile for the Controls tab: recording state + how many captures
 * are waiting to be collected. Tapping it opens the capture screen
 * (app/(tabs)/device-state/capture.tsx), which owns the actual start/stop control.
 *
 * Does no reads or subscriptions of its own: Capture State is in the always-on
 * monitor set (use-ble-connection.ts), so this tile tracks a capture that ends by
 * itself. It has to be always-on rather than screen-scoped — a capture reaching
 * its length limit is a device-side push with nobody on the capture screen to
 * hear it, and scoping it there left this tile reading "Recording" forever after
 * the user navigated back.
 *
 * The count is NOT notifiable (files also arrive and vanish over USB, which the
 * device cannot observe), so it can lag until the capture screen polls it. That
 * is the honest trade: a stale count is cosmetic, a stale state is a lie.
 */
export function CaptureCard({ style }: { style?: ViewStyle }) {
    const { selectedDevice } = useBluetooth();
    const c = useThemeColors();

    const chars = selectedDevice?.characteristics;
    const stateInfo = chars?.[UUID_CAPTURE_STATE];
    if (!stateInfo) {
        // Firmware without the capture service (or discovery still running).
        return null;
    }

    const state = captureStateFromCode(decodeUint32OrNull(stateInfo.value));
    const count = decodeUint32OrNull(chars?.[UUID_CAPTURE_COUNT]?.value);

    return (
        <Link href="/(tabs)/device-state/capture" asChild>
            <Pressable accessibilityRole="button" accessibilityLabel="Capture details">
                <Card style={style}>
                    <View style={styles.row}>
                        <ThemedText type="defaultSemiBold">Capture</ThemedText>
                        <View style={styles.rowRight}>
                            <Badge label={captureStateLabel(state)} tone={captureStateTone(state)} />
                            <IconSymbol name="chevron.right" size={20} color={c.textMuted} />
                        </View>
                    </View>
                    <ThemedText type="caption">
                        {count == null
                            ? "Record audio + IMU on the device"
                            : count === 1
                                ? "1 capture on the device"
                                : `${count} captures on the device`}
                    </ThemedText>
                </Card>
            </Pressable>
        </Link>
    );
}

const styles = StyleSheet.create({
    row: {
        flexDirection: 'row',
        alignItems: 'center',
        justifyContent: 'space-between',
        gap: Spacing.sm,
        marginBottom: Spacing.xs,
    },
    rowRight: {
        flexDirection: 'row',
        alignItems: 'center',
        gap: Spacing.sm,
    },
});
