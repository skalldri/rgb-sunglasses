import { IconSymbol } from "@/components/ui/icon-symbol";
import { CharacteristicInfo } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { decodeBooleanFromBase64, encodeBooleanToBase64 } from "@/services/ble-value-codec";
import { Pressable } from "react-native";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
    onWrite: (charUuid: string, encoded: string, previous: string) => void;
}

/**
 * Compact per-animation "include in shuffle" toggle for the Controls rows (issue #243):
 * a pressable shuffle icon, tinted when the animation participates in shuffle and dimmed
 * when excluded. Parent-managed write contract (same as CharacteristicBoolean) because
 * UUID_SHUFFLE_INCLUDE_CHARACTERISTIC is reused across every animation service — only
 * the parent row knows which service to write through.
 */
export function ShuffleToggle({ charUuid, charInfo, onWrite }: Props) {
    const c = useThemeColors();
    let included = false;
    if (charInfo.value) {
        try {
            included = decodeBooleanFromBase64(charInfo.value);
        } catch (e) {
            console.log('Error decoding shuffle-include value:', e);
        }
    }

    return (
        <Pressable
            testID="shuffle-toggle"
            accessibilityRole="switch"
            accessibilityLabel="Include in shuffle"
            accessibilityState={{ checked: included, disabled: charInfo.isUpdateInProgress }}
            disabled={charInfo.isUpdateInProgress}
            hitSlop={8}
            style={charInfo.isUpdateInProgress ? { opacity: 0.4 } : undefined}
            onPress={() => {
                const previousValue = charInfo.value ?? '';
                const encoded = encodeBooleanToBase64(!included);
                onWrite(charUuid, encoded, previousValue);
            }}
        >
            <IconSymbol name="shuffle" size={20} color={included ? c.primary : c.textMuted} />
        </Pressable>
    );
}
