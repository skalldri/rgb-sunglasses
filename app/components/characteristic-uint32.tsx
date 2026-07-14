import { CharacteristicInfo } from "@/context/bluetooth-context";
import { decodeUint32FromBase64, encodeUint32ToBase64, sanitizeNumericInput } from "@/services/ble-value-codec";
import { CharacteristicTextInputBase } from "./characteristic-text-input-base";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
    pendingValue: string;
    onChangeText: (charUuid: string, text: string) => void;
    onWrite: (charUuid: string, encoded: string, previous: string) => void;
}

export function CharacteristicUint32(props: Props) {
    return (
        <CharacteristicTextInputBase
            {...props}
            placeholder="Enter number"
            keyboardType="numeric"
            sanitize={sanitizeNumericInput}
            parseAndEncode={(text) => {
                const numericValue = parseInt(text, 10);
                if (isNaN(numericValue)) {
                    console.log(`Invalid number input: ${text}`);
                    return null;
                }
                return encodeUint32ToBase64(numericValue);
            }}
            decodeToDisplay={(encoded) => String(decodeUint32FromBase64(encoded))}
        />
    );
}
