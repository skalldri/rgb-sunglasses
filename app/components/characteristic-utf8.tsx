import { CharacteristicInfo } from "@/context/bluetooth-context";
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
    return (
        <TextInput
            style={styles.textInput}
            placeholder="Enter value"
            placeholderTextColor="#888"
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
        borderColor: '#ccc',
        borderRadius: 4,
        padding: 4,
        flex: 1,
        minWidth: 80,
        color: '#fff',
    },
});
