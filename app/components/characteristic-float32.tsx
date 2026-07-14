import { CharacteristicInfo } from "@/context/bluetooth-context";
import { decodeFloat32FromBase64, encodeFloat32ToBase64, formatFloat32, sanitizeFloatInput } from "@/services/ble-value-codec";
import { CharacteristicTextInputBase } from "./characteristic-text-input-base";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
    pendingValue: string;
    onChangeText: (charUuid: string, text: string) => void;
    onWrite: (charUuid: string, encoded: string, previous: string) => void;
}

export function CharacteristicFloat32(props: Props) {
    return (
        <CharacteristicTextInputBase
            {...props}
            placeholder="Enter number"
            keyboardType="decimal-pad"
            sanitize={sanitizeFloatInput}
            parseAndEncode={(text) => {
                const floatValue = parseFloat(text);
                if (isNaN(floatValue)) {
                    console.log(`Invalid number input: ${text}`);
                    return null;
                }
                return encodeFloat32ToBase64(floatValue);
            }}
            decodeToDisplay={(encoded) => formatFloat32(decodeFloat32FromBase64(encoded))}
        />
    );
}
