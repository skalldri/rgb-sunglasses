import { Radii, Spacing } from "@/constants/theme";
import { CharacteristicInfo } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { encodeUtf8ToBase64 } from "@/services/ble-value-codec";
import { StyleSheet, TextInput } from "react-native";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
    pendingValue: string;
    onChangeText: (charUuid: string, text: string) => void;
    onWrite: (charUuid: string, encoded: string, previous: string) => void;
}

export function CharacteristicUtf8({ charUuid, charInfo, pendingValue, onChangeText, onWrite }: Props) {
    const c = useThemeColors();
    return (
        <TextInput
            style={[styles.textInput, { color: c.textPrimary, backgroundColor: c.surfaceAlt, borderColor: c.border }]}
            placeholder="Enter value"
            placeholderTextColor={c.textMuted}
            editable={!charInfo.isUpdateInProgress}
            value={pendingValue}
            onChangeText={(text) => onChangeText(charUuid, text)}
            onSubmitEditing={() => {
                const previousValue = charInfo.value ?? '';
                const encoded = encodeUtf8ToBase64(pendingValue);
                onWrite(charUuid, encoded, previousValue);
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
