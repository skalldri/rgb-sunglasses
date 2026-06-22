import { Radii, Spacing } from "@/constants/theme";
import { CharacteristicInfo } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { encodeFloat32ToBase64, sanitizeFloatInput } from "@/services/ble-value-codec";
import { StyleSheet, TextInput } from "react-native";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
    pendingValue: string;
    onChangeText: (charUuid: string, text: string) => void;
    onWrite: (charUuid: string, encoded: string, previous: string) => void;
}

export function CharacteristicFloat32({ charUuid, charInfo, pendingValue, onChangeText, onWrite }: Props) {
    const c = useThemeColors();
    return (
        <TextInput
            style={[styles.textInput, { color: c.textPrimary, backgroundColor: c.surfaceAlt, borderColor: c.border }]}
            placeholder="Enter number"
            placeholderTextColor={c.textMuted}
            keyboardType="decimal-pad"
            editable={!charInfo.isUpdateInProgress}
            value={pendingValue}
            onChangeText={(text) => {
                const floatText = sanitizeFloatInput(text);
                onChangeText(charUuid, floatText);
            }}
            onSubmitEditing={() => {
                const previousValue = charInfo.value ?? '';
                const floatValue = parseFloat(pendingValue);
                if (!isNaN(floatValue)) {
                    const encoded = encodeFloat32ToBase64(floatValue);
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
