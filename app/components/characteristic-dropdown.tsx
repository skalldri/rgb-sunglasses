import { CharacteristicInfo, useBluetooth } from "@/context/bluetooth-context";
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
                style={styles.trigger}
                onPress={() => setIsOpen(prev => !prev)}
                accessibilityRole="button"
                accessibilityLabel={`${selected}, tap to change`}
            >
                <ThemedText style={styles.selectedText}>{selected}</ThemedText>
                <ThemedText style={styles.caret}>{isOpen ? '▲' : '▼'}</ThemedText>
            </Pressable>
            {isOpen && (
                <View style={styles.optionsList}>
                    {options.map(option => (
                        <Pressable key={option} style={styles.option} onPress={() => selectOption(option)}>
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
        borderColor: '#ccc',
        borderRadius: 6,
        paddingVertical: 8,
        paddingHorizontal: 12,
    },
    selectedText: {
        fontSize: 14,
    },
    caret: {
        fontSize: 10,
        marginLeft: 8,
    },
    optionsList: {
        borderWidth: 1,
        borderColor: '#ccc',
        borderTopWidth: 0,
        borderBottomLeftRadius: 6,
        borderBottomRightRadius: 6,
    },
    option: {
        paddingVertical: 10,
        paddingHorizontal: 12,
        borderTopWidth: 1,
        borderTopColor: '#ccc',
    },
    optionText: {
        fontSize: 14,
    },
    selectedOptionText: {
        fontSize: 14,
        fontWeight: 'bold',
    },
});
