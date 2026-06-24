import { Radii, Spacing } from "@/constants/theme";
import { CharacteristicInfo } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { encodeUint32ToBase64, sanitizeNumericInput } from "@/services/ble-value-codec";
import { StyleSheet, TextInput } from "react-native";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
    pendingValue: string;
    onChangeText: (charUuid: string, text: string) => void;
    onWrite: (charUuid: string, encoded: string, previous: string) => void;
}

export function CharacteristicUint32({ charUuid, charInfo, pendingValue, onChangeText, onWrite }: Props) {
    const c = useThemeColors();
    return (
        <TextInput
            style={[styles.textInput, { color: c.textPrimary, backgroundColor: c.surfaceAlt, borderColor: c.border }]}
            placeholder="Enter number"
            placeholderTextColor={c.textMuted}
            keyboardType="numeric"
            editable={!charInfo.isUpdateInProgress}
            value={pendingValue}
            onChangeText={(text) => {
                const numericText = sanitizeNumericInput(text);
                onChangeText(charUuid, numericText);
            }}
            onSubmitEditing={() => {
                const previousValue = charInfo.value ?? '';
                const numericValue = parseInt(pendingValue, 10);
                if (!isNaN(numericValue)) {
                    const encoded = encodeUint32ToBase64(numericValue);
                    onWrite(charUuid, encoded, previousValue);
                } else {
                    console.log(`Invalid number input: ${pendingValue}`);
                }
            }}
        />
    );
}

const styles = StyleSheet.create({
    textInput: {
        borderWidth: 1,
        borderRadius: Radii.md,
        paddingHorizontal: Spacing.sm,
        paddingVertical: Spacing.xs,
        flex: 1,
        minWidth: 80,
    },
});
