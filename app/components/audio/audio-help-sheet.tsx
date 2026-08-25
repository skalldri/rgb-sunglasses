import { Modal, Pressable, ScrollView, StyleSheet } from "react-native";

import { ThemedText } from "@/components/themed-text";
import { AppButton } from "@/components/ui/app-button";
import { Radii, Spacing } from "@/constants/theme";
import { useThemeColors } from "@/hooks/use-theme-color";

export interface AudioHelpContent {
    title: string;
    /** The firmware's own name for this parameter, so it greps against `sound dsp params`. */
    firmwareLabel?: string;
    body: string;
}

interface Props {
    content: AudioHelpContent | null;
    onClose: () => void;
}

/**
 * Per-parameter help.
 *
 * Deliberately a Modal rather than a pushed route: the live meters stay mounted behind it, so
 * you can read what a parameter does while still watching the thing it describes. Navigating
 * away would unmount the telemetry subscription and lose exactly the context that makes the
 * explanation land.
 */
export function AudioHelpSheet({ content, onClose }: Props) {
    const c = useThemeColors();

    return (
        <Modal
            visible={content !== null}
            transparent
            animationType="fade"
            onRequestClose={onClose}
        >
            <Pressable
                style={[styles.backdrop, { backgroundColor: c.overlay }]}
                accessibilityLabel="Close help"
                onPress={onClose}
            >
                {/* Swallow presses inside the card so tapping the text does not dismiss. */}
                <Pressable
                    style={[styles.card, { backgroundColor: c.surface, borderColor: c.border }]}
                    onPress={() => {}}
                    testID="audio-help-sheet"
                >
                    <ThemedText type="heading">{content?.title ?? ""}</ThemedText>
                    {content?.firmwareLabel ? (
                        <ThemedText type="caption" style={{ color: c.textMuted }}>
                            Firmware name: {content.firmwareLabel}
                        </ThemedText>
                    ) : null}

                    <ScrollView style={styles.bodyScroll} contentContainerStyle={styles.bodyContent}>
                        <ThemedText style={{ color: c.textSecondary, lineHeight: 21 }}>
                            {content?.body ?? ""}
                        </ThemedText>
                    </ScrollView>

                    <AppButton title="Got it" onPress={onClose} />
                </Pressable>
            </Pressable>
        </Modal>
    );
}

const styles = StyleSheet.create({
    backdrop: { flex: 1, alignItems: "center", justifyContent: "center", padding: Spacing.lg },
    card: {
        width: "100%",
        maxWidth: 460,
        maxHeight: "70%",
        borderRadius: Radii.lg,
        borderWidth: 1,
        padding: Spacing.lg,
        gap: Spacing.md,
    },
    bodyScroll: { flexGrow: 0 },
    bodyContent: { paddingBottom: Spacing.xs },
});
