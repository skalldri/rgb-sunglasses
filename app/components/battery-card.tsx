import { ThemedText } from "@/components/themed-text";
import { Badge } from "@/components/ui/badge";
import { Card } from "@/components/ui/card";
import { IconSymbol } from "@/components/ui/icon-symbol";
import { ProgressBar } from "@/components/ui/progress-bar";
import {
    UUID_BATTERY_CHARGE_STATUS, UUID_BATTERY_PERCENT, UUID_BATTERY_VOLTAGE, UUID_POWER_FLAGS,
} from "@/constants/bluetooth";
import { Spacing } from "@/constants/theme";
import { useBluetooth } from "@/context/bluetooth-context";
import {
    batteryPresent, CHARGE_STATUS_COMM_ERROR, chargeStatusLabel, voltageToPercent,
} from "@/services/battery";
import { decodeSint32FromBase64, decodeUint8FromBase64 } from "@/services/ble-value-codec";
import { useThemeColors } from "@/hooks/use-theme-color";
import { Link } from "expo-router";
import React from "react";
import { Pressable, StyleSheet, View, ViewStyle } from "react-native";

function decodeSint32OrNull(encoded: string | null | undefined): number | null {
    if (!encoded) return null;
    try {
        return decodeSint32FromBase64(encoded);
    } catch {
        return null;
    }
}

function decodeUint8OrNull(encoded: string | null | undefined): number | null {
    if (!encoded) return null;
    try {
        return decodeUint8FromBase64(encoded);
    } catch {
        return null;
    }
}

/** Badge tone for a raw BQ25792 CHG_STAT value (mirrors chargeDirection's buckets). */
function chargeStatusTone(chgStat: number): 'success' | 'info' | 'neutral' | 'warning' {
    if (chgStat === CHARGE_STATUS_COMM_ERROR) return 'warning'; // charger unreachable
    if (chgStat === 7) return 'info'; // Charge Done
    if ((chgStat >= 1 && chgStat <= 4) || chgStat === 6) return 'success'; // charging states
    return 'neutral'; // Not Charging / Reserved
}

/**
 * Compact battery tile for the Controls tab: charge-status badge + battery
 * percentage. Tapping it opens the battery detail page
 * (app/(tabs)/device-state/battery.tsx), which carries the full telemetry,
 * the charging toggle/charge-current setting, and the Power Debug section.
 *
 * Percentage prefers the firmware's own "Battery Percent" characteristic
 * (position 7, PR E) and falls back to the app-side voltageToPercent() curve
 * for older firmware that only exposes raw voltage.
 */
export function BatteryCard({ style }: { style?: ViewStyle }) {
    const { selectedDevice } = useBluetooth();
    const c = useThemeColors();

    const chars = selectedDevice?.characteristics;
    const fwPercent = decodeUint8OrNull(chars?.[UUID_BATTERY_PERCENT]?.value);
    const vbatMv = decodeSint32OrNull(chars?.[UUID_BATTERY_VOLTAGE]?.value);
    const chgStat = decodeUint8OrNull(chars?.[UUID_BATTERY_CHARGE_STATUS]?.value);
    const powerFlags = decodeUint8OrNull(chars?.[UUID_POWER_FLAGS]?.value);
    const hasBattery = batteryPresent(powerFlags, vbatMv);

    const percent = fwPercent ?? (vbatMv != null ? voltageToPercent(vbatMv) : null);

    if (percent == null) {
        // No percent characteristic and no voltage to derive one from —
        // nothing meaningful to show.
        return null;
    }

    return (
        <Link href="/(tabs)/device-state/battery" asChild>
            <Pressable accessibilityRole="button" accessibilityLabel="Battery details">
                <Card style={style}>
                    <View style={styles.row}>
                        <ThemedText type="defaultSemiBold">Battery</ThemedText>
                        <View style={styles.rowRight}>
                            {/* Comm error outranks No Battery: powerFlags is a
                                stale last-good reading during an outage, so
                                battery presence is unknowable then. */}
                            {chgStat === CHARGE_STATUS_COMM_ERROR ? (
                                <Badge label="Error" tone="warning" />
                            ) : !hasBattery ? (
                                <Badge label="No Battery" tone="danger" />
                            ) : chgStat != null && (
                                <Badge label={chargeStatusLabel(chgStat)} tone={chargeStatusTone(chgStat)} />
                            )}
                            {/* Chevron affordance, matching MenuRow's tap-through hint. */}
                            <IconSymbol name="chevron.right" size={20} color={c.textMuted} />
                        </View>
                    </View>
                    <ProgressBar progress={percent / 100} label={`${percent}%`} />
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
        marginBottom: Spacing.sm,
    },
    rowRight: {
        flexDirection: 'row',
        alignItems: 'center',
        gap: Spacing.sm,
    },
});
