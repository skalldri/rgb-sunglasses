import { IconSymbol } from "@/components/ui/icon-symbol";
import { WriteErrorIndicator } from "@/components/characteristic-write-error";
import { UUID_SHUFFLE_ENABLED } from "@/constants/bluetooth";
import { Spacing } from "@/constants/theme";
import { useBluetooth } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { decodeBooleanFromBase64, encodeBooleanToBase64 } from "@/services/ble-value-codec";
import { Pressable, StyleSheet, View } from "react-native";

/**
 * Music-player-style shuffle on/off button for the Controls page's Animations header
 * (issue #243), hard-coded to the Shuffle service's Enabled characteristic
 * (UUID_SHUFFLE_ENABLED — globally unique, so the flat characteristics map is legal
 * here, BatteryCard precedent). Self-writing via writeToCharacteristic with the default
 * optimistic-update + compare-and-swap-revert contract; renders nothing on firmware
 * without the Shuffle service.
 */
export function ShuffleButton() {
    const { selectedDevice, writeToCharacteristic } = useBluetooth();
    const c = useThemeColors();

    const charInfo = selectedDevice?.characteristics?.[UUID_SHUFFLE_ENABLED];
    if (!charInfo) {
        return null;
    }

    let enabled = false;
    if (charInfo.value) {
        try {
            enabled = decodeBooleanFromBase64(charInfo.value);
        } catch (e) {
            console.log('Error decoding shuffle-enabled value:', e);
        }
    }

    return (
        <View style={styles.row}>
            <WriteErrorIndicator charInfo={charInfo} />
            <Pressable
                testID="shuffle-button"
                accessibilityRole="switch"
                accessibilityLabel="Shuffle"
                accessibilityState={{ checked: enabled, disabled: charInfo.isUpdateInProgress }}
                disabled={charInfo.isUpdateInProgress}
                hitSlop={8}
                style={[
                    styles.button,
                    enabled && { backgroundColor: c.surfaceAlt },
                    charInfo.isUpdateInProgress && { opacity: 0.4 },
                ]}
                onPress={() => {
                    writeToCharacteristic(UUID_SHUFFLE_ENABLED, encodeBooleanToBase64(!enabled));
                }}
            >
                <IconSymbol name="shuffle" size={20} color={enabled ? c.primary : c.textMuted} />
            </Pressable>
        </View>
    );
}

const styles = StyleSheet.create({
    row: {
        flexDirection: 'row',
        alignItems: 'center',
        gap: Spacing.sm,
    },
    button: {
        padding: Spacing.xs,
        borderRadius: 8,
    },
});
