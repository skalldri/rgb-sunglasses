import { CharacteristicInfo } from "@/context/bluetooth-context";
import { decodeColorFromBase64 } from "@/services/ble-value-codec";
import { Link } from "expo-router";
import { Button, StyleSheet, View } from "react-native";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
}

export function CharacteristicColor({ charUuid, charInfo }: Props) {
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
            <View style={[styles.colorPreview, { backgroundColor: `rgb(${r}, ${g}, ${b})` }]} />
            <Link href={`/color-picker-modal?r=${r}&g=${g}&b=${b}&charUuid=${charUuid}`} asChild>
                <Button title="Pick Color" onPress={() => { }} />
            </Link>
        </View>
    );
}

const styles = StyleSheet.create({
    colorPickerContainer: {
        flexDirection: 'row',
        alignItems: 'center',
        gap: 8,
    },
    colorPreview: {
        width: 32,
        height: 32,
        borderRadius: 6,
        borderWidth: 1,
        borderColor: '#ccc',
    },
});
