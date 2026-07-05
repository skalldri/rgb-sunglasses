import { IconSymbol } from "@/components/ui/icon-symbol";
import { CharacteristicInfo } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { Alert, Pressable } from "react-native";

interface Props {
    charInfo: CharacteristicInfo;
}

/**
 * Per-row indicator for a characteristic whose last write failed (issue #92). Renders nothing when
 * there is no pending failure; otherwise a tappable warning icon that surfaces the reason in a
 * native Alert. The failure state lives on charInfo.lastWriteError in the Bluetooth context, so
 * this works uniformly on the Controls list, the detail page, and after a modal-initiated write
 * settles - wherever a characteristic row is rendered.
 */
export function WriteErrorIndicator({ charInfo }: Props) {
    const c = useThemeColors();
    const message = charInfo.lastWriteError;

    if (!message) return null;

    return (
        <Pressable
            onPress={() => Alert.alert('Write failed', message)}
            accessibilityRole="button"
            accessibilityLabel="Write failed, tap for details"
            testID="write-error-indicator"
            hitSlop={8}
        >
            <IconSymbol name="exclamationmark.triangle.fill" size={20} color={c.danger} />
        </Pressable>
    );
}
