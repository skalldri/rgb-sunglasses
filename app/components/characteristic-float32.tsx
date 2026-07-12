import { Radii, Spacing } from "@/constants/theme";
import { CharacteristicInfo } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { encodeFloat32ToBase64, sanitizeFloatInput } from "@/services/ble-value-codec";
import { Platform, StyleSheet, TextInput } from "react-native";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
    pendingValue: string;
    onChangeText: (charUuid: string, text: string) => void;
    onWrite: (charUuid: string, encoded: string, previous: string) => void;
}

export function CharacteristicFloat32({ charUuid, charInfo, pendingValue, onChangeText, onWrite }: Props) {
    const c = useThemeColors();
    const submit = (skipIfUnchanged: boolean) => {
        const previousValue = charInfo.value ?? '';
        const floatValue = parseFloat(pendingValue);
        if (isNaN(floatValue)) {
            console.log(`Invalid number input: ${pendingValue}`);
            return;
        }
        const encoded = encodeFloat32ToBase64(floatValue);
        if (skipIfUnchanged && encoded === previousValue) {
            return;
        }
        onWrite(charUuid, encoded, previousValue);
    };
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
            onSubmitEditing={() => submit(false)}
            // iOS's decimal pad has no return key, so onSubmitEditing is unreachable
            // there — ending the edit (tap outside / keyboard dismissed / focus moved)
            // is the commit signal instead, skipping no-op edits to avoid pointless
            // BLE writes on a casual tap-in/tap-out. Android keeps its explicit
            // ✓-submits / tap-away-cancels semantics.
            onEndEditing={Platform.OS === "ios" ? () => submit(true) : undefined}
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
