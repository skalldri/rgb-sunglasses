import { CharacteristicBoolean } from "@/components/characteristic-boolean";
import { Badge } from "@/components/ui/badge";
import { Card } from "@/components/ui/card";
import { Divider } from "@/components/ui/divider";
import { ListRow } from "@/components/ui/list-row";
import { ProgressBar } from "@/components/ui/progress-bar";
import { Section } from "@/components/ui/section";
import { ThemedText } from "@/components/themed-text";
import {
    UUID_BATTERY_CHARGE_ENABLE, UUID_BATTERY_CHARGE_STATUS, UUID_BATTERY_CURRENT,
    UUID_BATTERY_VBUS_CURRENT, UUID_BATTERY_VBUS_VOLTAGE, UUID_BATTERY_VOLTAGE,
} from "@/constants/bluetooth";
import { useBluetooth } from "@/context/bluetooth-context";
import {
    batteryWatts, chargeDirection, chargeStatusLabel, formatVolts, formatWatts, systemWatts,
    voltageToPercent, type ChargeDirection,
} from "@/services/battery";
import { decodeSint32FromBase64, decodeUint8FromBase64 } from "@/services/ble-value-codec";
import React from "react";
import { StyleSheet, ViewStyle } from "react-native";

function decodeSint32OrNull(encoded: string | null | undefined): number | null {
    if (!encoded) return null;
    try {
        return decodeSint32FromBase64(encoded);
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
 * Battery summary card for the Controls tab (issue #97). Reads the Battery
 * service's raw telemetry characteristics (all UUID-unique, so the flat
 * characteristics map applies) and derives percentage, direction, and watts
 * app-side. Values update live: every notifiable characteristic is
 * auto-monitored at connect time (use-ble-connection.ts), and each
 * notification lands here through context state.
 */
export function BatteryCard({ style }: { style?: ViewStyle }) {
    const { selectedDevice, writeToCharacteristic } = useBluetooth();

    const chars = selectedDevice?.characteristics;
    const vbatMv = decodeSint32OrNull(chars?.[UUID_BATTERY_VOLTAGE]?.value);
    const ibatMa = decodeSint32OrNull(chars?.[UUID_BATTERY_CURRENT]?.value);
    const vbusMv = decodeSint32OrNull(chars?.[UUID_BATTERY_VBUS_VOLTAGE]?.value);
    const ibusMa = decodeSint32OrNull(chars?.[UUID_BATTERY_VBUS_CURRENT]?.value);

    let chgStat: number | null = null;
    const chgStatValue = chars?.[UUID_BATTERY_CHARGE_STATUS]?.value;
    if (chgStatValue) {
        try {
            chgStat = decodeUint8FromBase64(chgStatValue);
        } catch { /* leave null */ }
    }

    const chargeEnableInfo = chars?.[UUID_BATTERY_CHARGE_ENABLE] ?? null;

    if (vbatMv == null) {
        // Voltage is the anchor value — without it there is nothing meaningful to show.
        return null;
    }

    const percent = voltageToPercent(vbatMv);
    const direction = ibatMa != null && chgStat != null ? chargeDirection(ibatMa, chgStat) : null;
    const badge = direction ? DIRECTION_BADGE[direction] : null;

    return (
        <Card style={style}>
            <Section title="Battery">
                <ProgressBar
                    progress={percent / 100}
                    label={`${percent}% • ${formatVolts(vbatMv)}`}
                />
                {ibatMa != null && (
                    <ListRow label="Current" sublabel={chgStat != null ? chargeStatusLabel(chgStat) : undefined}>
                        <ThemedText type="defaultSemiBold">{`${ibatMa} mA`}</ThemedText>
                        {badge && <Badge label={badge.label} tone={badge.tone} />}
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
                {chargeEnableInfo && (
                    <>
                        <Divider style={styles.divider} />
                        <ListRow label="Charging Enabled">
                            <CharacteristicBoolean
                                charUuid={UUID_BATTERY_CHARGE_ENABLE}
                                charInfo={chargeEnableInfo}
                                onWrite={(charUuid, encoded) => writeToCharacteristic(charUuid, encoded)}
                            />
                        </ListRow>
                    </>
                )}
            </Section>
        </Card>
    );
}

const styles = StyleSheet.create({
    divider: {
        marginVertical: 4,
    },
});
