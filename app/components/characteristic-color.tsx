import { AppButton } from "@/components/ui/app-button";
import { Radii, Spacing } from "@/constants/theme";
import { CharacteristicInfo } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { decodeColorFromBase64 } from "@/services/ble-value-codec";
import { Link } from "expo-router";
import { StyleSheet, View } from "react-native";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
}

export function CharacteristicColor({ charUuid, charInfo }: Props) {
    const c = useThemeColors();
    let r = 0, g = 0, b = 0;
    try {
        const color = decodeColorFromBase64(charInfo.value);
        r = color.r;
        g = color.g;
        b = color.b;
    } catch (e) {
        console.log('Error decoding custom color value:', e);
    }

    return (
        <View style={styles.colorPickerContainer}>
            <View style={[styles.colorPreview, { backgroundColor: `rgb(${r}, ${g}, ${b})`, borderColor: c.border }]} />
            <Link href={`/color-picker-modal?r=${r}&g=${g}&b=${b}&charUuid=${charUuid}`} asChild>
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
