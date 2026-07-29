import { Badge } from "@/components/ui/badge";
import { AppButton } from "@/components/ui/app-button";
import { COLOR_MODE_LABELS, COLOR_MODE_STATIC } from "@/constants/bluetooth";
import { Radii, Spacing } from "@/constants/theme";
import { CharacteristicInfo } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { ColorValue, decodeColorValueFromBase64 } from "@/services/ble-value-codec";
import { Link } from "expo-router";
import { StyleSheet, View } from "react-native";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
}

export function CharacteristicColor({ charUuid, charInfo }: Props) {
    const c = useThemeColors();
    let value: ColorValue = { mode: COLOR_MODE_STATIC, rgb: { r: 0, g: 0, b: 0 }, speed: 0 };
    try {
        value = decodeColorValueFromBase64(charInfo.value);
    } catch (e) {
        console.log('Error decoding custom color value:', e);
    }
    const { mode, rgb: { r, g, b }, speed } = value;

    return (
        <View style={styles.colorPickerContainer}>
            {mode === COLOR_MODE_STATIC ? (
                <View style={[styles.colorPreview, { backgroundColor: `rgb(${r}, ${g}, ${b})`, borderColor: c.border }]} />
            ) : (
                // In special modes the panel color is firmware-computed; the mode
                // label is the meaningful thing to show, not a swatch.
                <Badge label={COLOR_MODE_LABELS[mode]} tone="info" />
            )}
            <Link href={`/color-picker-modal?mode=${mode}&r=${r}&g=${g}&b=${b}&speed=${speed}&charUuid=${charUuid}`} asChild>
                <AppButton title="Pick Color" variant="secondary" />
            </Link>
        </View>
    );
}

const styles = StyleSheet.create({
    colorPickerContainer: {
        flexDirection: 'row',
        alignItems: 'center',
        gap: Spacing.sm,
    },
    colorPreview: {
        width: 32,
        height: 32,
        borderRadius: Radii.sm,
        borderWidth: 1,
    },
});
