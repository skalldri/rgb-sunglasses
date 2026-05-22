import { CharacteristicInfo } from "@/context/bluetooth-context";
import { decodeBooleanFromBase64, encodeBooleanToBase64 } from "@/services/ble-value-codec";
import { Switch } from "react-native";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
    onWrite: (charUuid: string, encoded: string, previous: string) => void;
}

export function CharacteristicBoolean({ charUuid, charInfo, onWrite }: Props) {
    let displayValue = false;
    if (charInfo.value) {
        try {
            displayValue = decodeBooleanFromBase64(charInfo.value);
        } catch (e) {
            console.log('Error decoding boolean value:', e);
        }
    }

    return (
        <Switch
            value={displayValue}
            disabled={charInfo.isUpdateInProgress}
            onValueChange={(value) => {
                const previousValue = charInfo.value ?? '';
                const encoded = encodeBooleanToBase64(value);
                onWrite(charUuid, encoded, previousValue);
            }}
        />
    );
}
