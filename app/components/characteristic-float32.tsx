import { CharacteristicInfo } from "@/context/bluetooth-context";
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
    return (
        <TextInput
            style={styles.textInput}
            placeholder="Enter number"
            placeholderTextColor="#888"
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
        borderColor: '#ccc',
        borderRadius: 4,
        padding: 4,
        flex: 1,
        minWidth: 80,
        color: '#fff',
    },
});
