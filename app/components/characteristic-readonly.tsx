import { ThemedText } from "@/components/themed-text";
import {
    BLE_GATT_CPF_FORMAT_BOOLEAN, BLE_GATT_CPF_FORMAT_FLOAT32, BLE_GATT_CPF_FORMAT_SINT32,
    BLE_GATT_CPF_FORMAT_UINT8, BLE_GATT_CPF_FORMAT_UINT32, BLE_GATT_CPF_FORMAT_UTF8S,
} from "@/constants/bluetooth";
import { CharacteristicInfo } from "@/context/bluetooth-context";
import {
    decodeBooleanFromBase64, decodeFloat32FromBase64, decodeSint32FromBase64,
    decodeUint8FromBase64, decodeUint32FromBase64, decodeUtf8FromBase64, formatFloat32,
} from "@/services/ble-value-codec";

/**
 * Formats a base64 characteristic value as display text according to its CPF format.
 * Exported separately from the component so it's unit-testable without rendering.
 */
export function formatReadonlyValue(cpfFormat: number | null, encodedValue: string | null): string {
    if (encodedValue == null) return "—";
    try {
        switch (cpfFormat) {
            case BLE_GATT_CPF_FORMAT_BOOLEAN:
                return decodeBooleanFromBase64(encodedValue) ? "On" : "Off";
            case BLE_GATT_CPF_FORMAT_UINT8:
                return String(decodeUint8FromBase64(encodedValue));
            case BLE_GATT_CPF_FORMAT_UINT32:
                return String(decodeUint32FromBase64(encodedValue));
            case BLE_GATT_CPF_FORMAT_SINT32:
                return String(decodeSint32FromBase64(encodedValue));
            case BLE_GATT_CPF_FORMAT_FLOAT32:
                return formatFloat32(decodeFloat32FromBase64(encodedValue));
            case BLE_GATT_CPF_FORMAT_UTF8S:
                return decodeUtf8FromBase64(encodedValue);
            default:
                // Unknown format: best-effort text decode (matches decodeValueForInput's fallback)
                return decodeUtf8FromBase64(encodedValue);
        }
    } catch (e) {
        console.log(`Error decoding read-only value (cpf ${cpfFormat}):`, e);
        return "—";
    }
}

interface Props {
    charInfo: CharacteristicInfo;
}

/**
 * Plain-text display for characteristics the device does not allow writing
 * (telemetry like battery voltage/current). Rendered by
 * renderCharacteristicInput() when the characteristic's declared properties
 * lack every write flag — possible since the firmware-side fix that stopped
 * ReadOnly characteristics advertising WRITE (see bt_service_cpp.h).
 */
export function CharacteristicReadonly({ charInfo }: Props) {
    return (
        <ThemedText type="defaultSemiBold">
            {formatReadonlyValue(charInfo.cpfFormat, charInfo.value)}
        </ThemedText>
    );
}
