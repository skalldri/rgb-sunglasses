import { CharacteristicBoolean } from "@/components/characteristic-boolean";
import { WriteErrorIndicator } from "@/components/characteristic-write-error";
import { ThemedText } from "@/components/themed-text";
import { AppButton } from "@/components/ui/app-button";
import { Badge } from "@/components/ui/badge";
import { Card } from "@/components/ui/card";
import { Divider } from "@/components/ui/divider";
import { EmptyState } from "@/components/ui/empty-state";
import { IconSymbol } from "@/components/ui/icon-symbol";
import { ListRow } from "@/components/ui/list-row";
import { ProgressBar } from "@/components/ui/progress-bar";
import { Section } from "@/components/ui/section";
import {
    getCharacteristicName,
    UUID_BATTERY_CHARGE_CURRENT, UUID_BATTERY_CHARGE_ENABLE, UUID_BATTERY_CHARGE_STATUS,
    UUID_BATTERY_CURRENT, UUID_BATTERY_PERCENT, UUID_BATTERY_SERVICE, UUID_BATTERY_VBUS_CURRENT,
    UUID_BATTERY_VBUS_VOLTAGE, UUID_BATTERY_VOLTAGE, UUID_POWER_DEBUG_SERVICE,
} from "@/constants/bluetooth";
import { Spacing } from "@/constants/theme";
import { useBluetooth } from "@/context/bluetooth-context";
import { useCharacteristicEditor } from "@/hooks/use-characteristic-editor";
import { useThemeColors } from "@/hooks/use-theme-color";
import {
    batteryWatts, chargeDirection, chargeStatusLabel, formatVolts, formatWatts, systemWatts,
    voltageToPercent, type ChargeDirection,
} from "@/services/battery";
import { decodeSint32FromBase64, decodeUint8FromBase64 } from "@/services/ble-value-codec";
import { Link, useRouter } from "expo-router";
import React from "react";
import { Pressable, ScrollView, StyleSheet, View } from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";

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

const DIRECTION_BADGE: Record<ChargeDirection, { label: string; tone: 'success' | 'neutral' | 'info' }> = {
    charging: { label: "Charging", tone: 'success' },
    discharging: { label: "Discharging", tone: 'neutral' },
    idle: { label: "Idle", tone: 'neutral' },
    done: { label: "Charge Done", tone: 'info' },
};

/**
 * Battery detail page (power plan PR E): full curated telemetry (voltage,
 * current, VBUS, watts, charge status), the Charging Enabled toggle and the
 * editable Charge Current setting, plus a generically-rendered "Power Debug"
 * section when the firmware exposes the Power Debug service. Reached by
 * tapping the compact BatteryCard tile on the Controls screen.
 */
export default function BatteryDetailScreen() {
    const router = useRouter();
    const { selectedDevice, writeToCharacteristic } = useBluetooth();
    const { renderCharacteristicInput, labelColorFor } = useCharacteristicEditor();
    const c = useThemeColors();

    const chars = selectedDevice?.characteristics;
    const vbatMv = decodeSint32OrNull(chars?.[UUID_BATTERY_VOLTAGE]?.value);
    const ibatMa = decodeSint32OrNull(chars?.[UUID_BATTERY_CURRENT]?.value);
    const vbusMv = decodeSint32OrNull(chars?.[UUID_BATTERY_VBUS_VOLTAGE]?.value);
    const ibusMa = decodeSint32OrNull(chars?.[UUID_BATTERY_VBUS_CURRENT]?.value);
    const chgStat = decodeUint8OrNull(chars?.[UUID_BATTERY_CHARGE_STATUS]?.value);
    const fwPercent = decodeUint8OrNull(chars?.[UUID_BATTERY_PERCENT]?.value);

    const chargeEnableInfo = chars?.[UUID_BATTERY_CHARGE_ENABLE] ?? null;
    const chargeCurrentInfo = chars?.[UUID_BATTERY_CHARGE_CURRENT] ?? null;
    const powerDebugChars = selectedDevice?.characteristicsByService?.[UUID_POWER_DEBUG_SERVICE] ?? null;

    const header = (
        <View style={styles.header}>
            <Pressable
                onPress={() => router.back()}
                accessibilityRole="button"
                accessibilityLabel="Back to Controls"
                hitSlop={8}
                style={styles.backButton}
            >
                <IconSymbol name="chevron.left" size={22} color={c.primary} />
                <ThemedText style={{ color: c.primary }}>Controls</ThemedText>
            </Pressable>
        </View>
    );

    if (selectedDevice == null || vbatMv == null) {
        return (
            <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
                {header}
                <EmptyState
                    icon="🔋"
                    title="Not available"
                    subtitle="This device isn't connected, or no longer exposes the battery service."
                    action={
                        <Link href="/(tabs)/device-state" asChild>
                            <AppButton title="Back to Controls" variant="primary" />
                        </Link>
                    }
                />
            </SafeAreaView>
        );
    }

    // Prefer the firmware's own percent (Battery Percent characteristic, PR E);
    // fall back to the app-side curve for older firmware.
    const percent = fwPercent ?? voltageToPercent(vbatMv);
    const direction = ibatMa != null && chgStat != null ? chargeDirection(ibatMa, chgStat) : null;
    const badge = direction ? DIRECTION_BADGE[direction] : null;

    return (
        <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
            {header}
            <ScrollView contentContainerStyle={styles.scrollContent}>
                <ThemedText type="heading">Battery</ThemedText>

                <Card style={styles.card}>
                    <Section>
                        <ProgressBar
                            progress={percent / 100}
                            label={`${percent}% • ${formatVolts(vbatMv)}`}
                        />
                        <ListRow label="Voltage">
                            <ThemedText type="defaultSemiBold">{formatVolts(vbatMv)}</ThemedText>
                        </ListRow>
                        {ibatMa != null && (
                            <ListRow label="Current">
                                <ThemedText type="defaultSemiBold">{`${ibatMa} mA`}</ThemedText>
                                {badge && <Badge label={badge.label} tone={badge.tone} />}
                            </ListRow>
                        )}
                        {vbusMv != null && (
                            <ListRow label="VBUS Voltage">
                                <ThemedText type="defaultSemiBold">{formatVolts(vbusMv)}</ThemedText>
                            </ListRow>
                        )}
                        {ibusMa != null && (
                            <ListRow label="VBUS Current">
                                <ThemedText type="defaultSemiBold">{`${ibusMa} mA`}</ThemedText>
                            </ListRow>
                        )}
                        {vbusMv != null && ibusMa != null && ibatMa != null && (
                            <ListRow label="System Power">
                                <ThemedText type="defaultSemiBold">
                                    {formatWatts(systemWatts(vbusMv, ibusMa, vbatMv, ibatMa))}
                                </ThemedText>
                            </ListRow>
                        )}
                        {ibatMa != null && (
                            <ListRow label="Battery Power">
                                <ThemedText type="defaultSemiBold">
                                    {formatWatts(Math.abs(batteryWatts(vbatMv, ibatMa)))}
                                </ThemedText>
                            </ListRow>
                        )}
                        {chgStat != null && (
                            <ListRow label="Charge Status">
                                <ThemedText type="defaultSemiBold">{chargeStatusLabel(chgStat)}</ThemedText>
                            </ListRow>
                        )}
                    </Section>
                </Card>

                {(chargeEnableInfo || chargeCurrentInfo) && (
                    <Card style={styles.card}>
                        <Section title="Charging">
                            {chargeEnableInfo && (
                                <ListRow label="Charging Enabled">
                                    <WriteErrorIndicator charInfo={chargeEnableInfo} />
                                    <CharacteristicBoolean
                                        charUuid={UUID_BATTERY_CHARGE_ENABLE}
                                        charInfo={chargeEnableInfo}
                                        onWrite={(charUuid, encoded) => writeToCharacteristic(charUuid, encoded)}
                                    />
                                </ListRow>
                            )}
                            {chargeEnableInfo && chargeCurrentInfo && <Divider />}
                            {chargeCurrentInfo && (
                                <ListRow
                                    label="Charge Current (mA)"
                                    labelColor={labelColorFor(UUID_BATTERY_CHARGE_CURRENT)}
                                >
                                    <WriteErrorIndicator charInfo={chargeCurrentInfo} />
                                    {renderCharacteristicInput(
                                        UUID_BATTERY_SERVICE,
                                        UUID_BATTERY_CHARGE_CURRENT,
                                        chargeCurrentInfo
                                    )}
                                </ListRow>
                            )}
                        </Section>
                    </Card>
                )}

                {powerDebugChars && Object.keys(powerDebugChars).length > 0 && (
                    <Card style={styles.card}>
                        <Section title="Power Debug">
                            {Object.entries(powerDebugChars).map(([charUuid, charInfo], charIndex) => (
                                <React.Fragment key={`power-debug-char-${charIndex}`}>
                                    {charIndex > 0 && <Divider />}
                                    <ListRow
                                        label={charInfo.name ?? getCharacteristicName(charUuid)}
                                        labelColor={labelColorFor(charUuid)}
                                    >
                                        <WriteErrorIndicator charInfo={charInfo} />
                                        {renderCharacteristicInput(UUID_POWER_DEBUG_SERVICE, charUuid, charInfo)}
                                    </ListRow>
                                </React.Fragment>
                            ))}
                        </Section>
                    </Card>
                )}
            </ScrollView>
        </SafeAreaView>
    );
}

const styles = StyleSheet.create({
    container: {
        flex: 1,
    },
    header: {
        paddingHorizontal: Spacing.lg,
        paddingTop: Spacing.lg,
        paddingBottom: Spacing.sm,
    },
    backButton: {
        flexDirection: 'row',
        alignItems: 'center',
        gap: Spacing.xs,
        alignSelf: 'flex-start',
    },
    scrollContent: {
        paddingHorizontal: Spacing.lg,
        paddingBottom: Spacing.xxl,
        gap: Spacing.md,
    },
    card: {
        marginBottom: 0,
    },
});
