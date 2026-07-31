import { ReactNode } from "react";
import { Animated, Pressable, StyleSheet, View } from "react-native";

import { ListRow } from "@/components/ui/list-row";
import { IconSymbol } from "@/components/ui/icon-symbol";
import { Radii, Spacing } from "@/constants/theme";
import { useThemeColors } from "@/hooks/use-theme-color";

interface Props {
    label: string;
    labelColor?: string | Animated.AnimatedInterpolation<string | number>;
    slotIndex: number;
    isNowPlaying: boolean;
    isUpNext: boolean;
    // Whether to render the up-next button at all (false when the service has no
    // SLOT_UP_NEXT characteristic — slot rows still render, minus the affordance).
    showUpNextButton: boolean;
    // The SLOT_UP_NEXT characteristic's isUpdateInProgress — disables every row's button
    // while a queue write is in flight (there's one shared up-next value per service).
    upNextDisabled: boolean;
    onQueueUpNext: () => void;
    // The slot's own value input (CharacteristicUtf8 via renderCharacteristicInput) plus
    // its WriteErrorIndicator — composed by the parent so all existing text-editing
    // machinery (pendingValues, commit semantics) is reused untouched.
    children: ReactNode;
}

/**
 * One row of the slot playlist (issue #260): the slot's text input, an up-next queue
 * button (ShuffleToggle's visual language: primary tint when this slot is queued, muted
 * otherwise), and a now-playing treatment (surfaceAlt background + primary accent bar +
 * play glyph — never color alone). The two highlight axes are independent, so a slot
 * that is playing AND queued to repeat shows both.
 */
export function CharacteristicSlotRow({
    label, labelColor, slotIndex, isNowPlaying, isUpNext, showUpNextButton, upNextDisabled,
    onQueueUpNext, children,
}: Props) {
    const c = useThemeColors();

    return (
        <View
            style={[styles.wrapper, isNowPlaying && { backgroundColor: c.surfaceAlt }]}
            accessibilityLabel={isNowPlaying ? `${label}, now playing` : undefined}
        >
            {isNowPlaying && <View style={[styles.accent, { backgroundColor: c.primary }]} />}
            {isNowPlaying && (
                <IconSymbol name="play.fill" size={14} color={c.primary} style={styles.playGlyph} />
            )}
            <View style={styles.rowWrap}>
                <ListRow label={label} labelColor={labelColor}>
                    {children}
                    {showUpNextButton && (
                        <Pressable
                            testID={`slot-up-next-${slotIndex}`}
                            accessibilityRole="button"
                            accessibilityLabel={`Play ${label} next`}
                            accessibilityState={{ selected: isUpNext, disabled: upNextDisabled }}
                            disabled={upNextDisabled}
                            hitSlop={8}
                            style={upNextDisabled ? { opacity: 0.4 } : undefined}
                            onPress={onQueueUpNext}
                        >
                            <IconSymbol
                                name="text.line.first.and.arrowtriangle.forward"
                                size={20}
                                color={isUpNext ? c.primary : c.textMuted}
                            />
                        </Pressable>
                    )}
                </ListRow>
            </View>
        </View>
    );
}

const styles = StyleSheet.create({
    wrapper: {
        flexDirection: 'row',
        alignItems: 'center',
        borderRadius: Radii.md,
        marginHorizontal: -Spacing.xs,
        paddingHorizontal: Spacing.xs,
    },
    accent: {
        width: 3,
        alignSelf: 'stretch',
        marginVertical: Spacing.xs,
        borderRadius: Radii.sm,
    },
    playGlyph: { marginLeft: Spacing.xs },
    rowWrap: { flex: 1 },
});
