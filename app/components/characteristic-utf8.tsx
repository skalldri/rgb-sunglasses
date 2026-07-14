import { CharacteristicInfo } from "@/context/bluetooth-context";
import { decodeUtf8FromBase64, encodeUtf8ToBase64 } from "@/services/ble-value-codec";
import { CharacteristicTextInputBase } from "./characteristic-text-input-base";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
    pendingValue: string;
    onChangeText: (charUuid: string, text: string) => void;
    onWrite: (charUuid: string, encoded: string, previous: string) => void;
}

export function CharacteristicUtf8(props: Props) {
    return (
        <CharacteristicTextInputBase
            {...props}
            placeholder="Enter value"
            parseAndEncode={(text) => encodeUtf8ToBase64(text)}
            decodeToDisplay={decodeUtf8FromBase64}
        />
    );
}
