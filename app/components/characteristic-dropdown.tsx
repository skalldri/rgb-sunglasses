import { Radii, Spacing } from "@/constants/theme";
import { CharacteristicInfo, useBluetooth } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { decodeUtf8FromBase64, encodeUtf8ToBase64 } from "@/services/ble-value-codec";
import { useState } from "react";
import { Pressable, StyleSheet, View } from "react-native";
import { ThemedText } from "./themed-text";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
}

export function CharacteristicDropdown({ charUuid, charInfo }: Props) {
    const { writeToCharacteristic } = useBluetooth();
    const c = useThemeColors();
    const [isOpen, setIsOpen] = useState(false);

    let options: string[] = [];
    try {
        const decoded = charInfo.value ? decodeUtf8FromBase64(charInfo.value) : '';
        options = decoded.split('\n').filter(option => option.length > 0);
    } catch (e) {
        console.log('Error decoding dropdown list value:', e);
    }

    const selected = options[0] ?? '';

    async function selectOption(option: string) {
        setIsOpen(false);
        if (option === selected) return;
        // The device reorders this into the canonical "selected\nother..." list and notifies
        // it back, so the bare option text we just wrote is not the characteristic's final
        // value — let the notification (already subscribed during connect) update local state.
        await writeToCharacteristic(charUuid, encodeUtf8ToBase64(option), { skipOptimisticUpdate: true });
    }

    return (
        <View style={styles.container}>
            <Pressable
                style={[styles.trigger, { backgroundColor: c.surfaceAlt, borderColor: c.border }]}
                onPress={() => setIsOpen(prev => !prev)}
                accessibilityRole="button"
                accessibilityLabel={`${selected}, tap to change`}
            >
                <ThemedText style={styles.selectedText}>{selected}</ThemedText>
                <ThemedText style={styles.caret}>{isOpen ? '▲' : '▼'}</ThemedText>
            </Pressable>
            {isOpen && (
                <View style={[styles.optionsList, { backgroundColor: c.surface, borderColor: c.border }]}>
                    {options.map(option => (
                        <Pressable
                            key={option}
                            style={[styles.option, { borderTopColor: c.border }]}
                            onPress={() => selectOption(option)}
                        >
                            <ThemedText style={option === selected ? styles.selectedOptionText : styles.optionText}>
                                {option}
                            </ThemedText>
                        </Pressable>
                    ))}
                </View>
            )}
        </View>
    );
}

const styles = StyleSheet.create({
    container: {
        flex: 1,
    },
    trigger: {
        flexDirection: 'row',
        alignItems: 'center',
        justifyContent: 'space-between',
        borderWidth: 1,
        borderRadius: Radii.md,
        paddingVertical: Spacing.sm,
        paddingHorizontal: Spacing.md,
    },
    selectedText: {
        fontSize: 14,
    },
    caret: {
        fontSize: 10,
        marginLeft: Spacing.sm,
    },
    optionsList: {
        borderWidth: 1,
        borderTopWidth: 0,
        borderBottomLeftRadius: Radii.md,
        borderBottomRightRadius: Radii.md,
    },
    option: {
        paddingVertical: Spacing.sm + 2,
        paddingHorizontal: Spacing.md,
        borderTopWidth: 1,
    },
    optionText: {
        fontSize: 14,
    },
    selectedOptionText: {
        fontSize: 14,
        fontWeight: 'bold',
    },
});
