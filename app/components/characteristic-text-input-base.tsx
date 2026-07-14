import { Radii, Spacing } from "@/constants/theme";
import { CharacteristicInfo } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { useRef } from "react";
import { KeyboardTypeOptions, Platform, StyleSheet, TextInput } from "react-native";

interface Props {
    charUuid: string;
    charInfo: CharacteristicInfo;
    pendingValue: string;
    placeholder: string;
    keyboardType?: KeyboardTypeOptions;
    /** Filters each keystroke (e.g. strip non-digits). Identity when omitted. */
    sanitize?: (text: string) => string;
    /** Parses + encodes the committed text; returns null for invalid input (no write). */
    parseAndEncode: (text: string) => string | null;
    /** Decodes a stored value to its canonical display string, for the no-op-edit check. */
    decodeToDisplay: (encodedValue: string) => string;
    onChangeText: (charUuid: string, text: string) => void;
    onWrite: (charUuid: string, encoded: string, previous: string) => void;
}

// Shared commit behavior for every text-based characteristic input:
//
//  - Android: the keyboard's ✓/Return fires onSubmitEditing; tap-away cancels.
//  - iOS: number-pad/decimal-pad keyboards have NO return key, so onSubmitEditing
//    alone is unreachable there — ending the edit (tap outside / keyboard
//    dismissed / focus moved) is the commit signal on iOS instead, for every
//    text input uniformly so tap-away behaves the same across field types.
//
// The no-op-edit check compares DISPLAY strings, not encoded bytes: for float32,
// the displayed value is rounded to 7 significant digits, so re-encoding it can
// differ from the stored bytes by 1 ULP even when the user typed nothing —
// a byte-level compare would turn a casual tap-in/tap-out into a real BLE write
// that corrupts the stored value.
//
// submittedRef suppresses the blur-after-submit double-fire: when a return key
// IS available (iOS text keyboard, any hardware keyboard), onSubmitEditing runs
// first and the resulting blur must not commit a second time.
export function CharacteristicTextInputBase({
    charUuid, charInfo, pendingValue, placeholder, keyboardType,
    sanitize, parseAndEncode, decodeToDisplay, onChangeText, onWrite,
}: Props) {
    const c = useThemeColors();
    const submittedRef = useRef(false);

    const commit = (skipIfUnchanged: boolean) => {
        const previousValue = charInfo.value ?? '';
        const encoded = parseAndEncode(pendingValue);
        if (encoded === null) {
            return;
        }
        if (skipIfUnchanged && previousValue) {
            let currentDisplay: string | null = null;
            try {
                currentDisplay = decodeToDisplay(previousValue);
            } catch {
                // Undecodable stored value: can't prove the edit is a no-op, so write.
            }
            if (currentDisplay !== null && currentDisplay === pendingValue) {
                return;
            }
        }
        onWrite(charUuid, encoded, previousValue);
    };

    return (
        <TextInput
            style={[styles.textInput, { color: c.textPrimary, backgroundColor: c.surfaceAlt, borderColor: c.border }]}
            placeholder={placeholder}
            placeholderTextColor={c.textMuted}
            keyboardType={keyboardType}
            editable={!charInfo.isUpdateInProgress}
            value={pendingValue}
            onChangeText={(text) => onChangeText(charUuid, sanitize ? sanitize(text) : text)}
            onSubmitEditing={() => {
                submittedRef.current = true;
                commit(false);
            }}
            onEndEditing={Platform.OS === "ios" ? () => {
                if (submittedRef.current) {
                    submittedRef.current = false;
                    return;
                }
                commit(true);
            } : undefined}
        />
    );
}

const styles = StyleSheet.create({
    textInput: {
        borderWidth: 1,
        borderRadius: Radii.md,
        paddingHorizontal: Spacing.sm,
        paddingVertical: Spacing.xs,
        flex: 1,
        minWidth: 80,
    },
});
