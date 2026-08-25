import Slider from "@react-native-community/slider";
import { memo, useCallback } from "react";
import { Pressable, StyleSheet, View } from "react-native";

import { ThemedText } from "@/components/themed-text";
import { IconSymbol } from "@/components/ui/icon-symbol";
import { Radii, Spacing } from "@/constants/theme";
import { useThemeColors } from "@/hooks/use-theme-color";
import {
    AudioParamSpec,
    formatParamValue,
    paramToPosition,
    positionToParam,
} from "@/services/audio-params";

interface Props {
    spec: AudioParamSpec;
    /** Current value, or null when the characteristic has not been read yet. */
    value: number | null;
    /** True while a write for this parameter is in flight. */
    busy?: boolean;
    /** Shown under the control when the firmware clamped the last write. */
    clampNote?: string | null;
    /** Live readout under the control, e.g. "Firing 2.1 times a second". */
    liveNote?: string | null;
    /** Show the firmware's own name in grey so it greps against `sound dsp params`. */
    showFirmwareLabel?: boolean;
    onSlide: (value: number) => void;
    onSlideComplete: (value: number) => void;
    onHelp?: () => void;
    disabled?: boolean;
}

/**
 * One tunable parameter as a labelled slider.
 *
 * Memoised on the values it actually renders. The Audio Tuning screen recomputes a live
 * telemetry summary at 2 Hz, and without this every one of those ticks would re-render all 14
 * controls — including the one currently under the user's finger.
 */
function ParamSliderRowImpl({
    spec,
    value,
    busy,
    clampNote,
    liveNote,
    showFirmwareLabel,
    onSlide,
    onSlideComplete,
    onHelp,
    disabled,
}: Props) {
    const c = useThemeColors();

    const position = value === null ? 0 : paramToPosition(spec, value);
    const readout = value === null ? "--" : formatParamValue(spec, value);

    const handleValueChange = useCallback(
        (p: number) => onSlide(positionToParam(spec, p)),
        [onSlide, spec],
    );
    const handleComplete = useCallback(
        (p: number) => onSlideComplete(positionToParam(spec, p)),
        [onSlideComplete, spec],
    );

    return (
        <View style={styles.wrap} testID={`param-row-${spec.key}`}>
            <View style={styles.header}>
                <View style={styles.labelWrap}>
                    <ThemedText style={styles.friendly}>{spec.friendlyLabel}</ThemedText>
                    {showFirmwareLabel ? (
                        <ThemedText type="caption" style={{ color: c.textMuted }}>
                            {spec.firmwareLabel}
                        </ThemedText>
                    ) : null}
                </View>

                <View style={styles.readoutWrap}>
                    {/* A dot, not an opacity change on the slider itself — dimming the track
                        while dragging strobes on every throttled write. */}
                    {busy ? <View style={[styles.busyDot, { backgroundColor: c.info }]} /> : null}
                    <ThemedText
                        style={[styles.readout, { color: c.textPrimary }]}
                        testID={`param-value-${spec.key}`}
                    >
                        {readout}
                    </ThemedText>
                    {onHelp ? (
                        <Pressable
                            accessibilityRole="button"
                            accessibilityLabel={`What does ${spec.friendlyLabel} do?`}
                            hitSlop={12}
                            onPress={onHelp}
                            testID={`param-help-${spec.key}`}
                            style={[styles.helpButton, { borderColor: c.border }]}
                        >
                            <IconSymbol name="questionmark" size={13} color={c.textSecondary} />
                        </Pressable>
                    ) : null}
                </View>
            </View>

            <Slider
                testID={`param-slider-${spec.key}`}
                accessibilityLabel={`${spec.friendlyLabel}, ${readout}`}
                style={styles.slider}
                minimumValue={0}
                maximumValue={1}
                value={position}
                disabled={disabled || value === null}
                minimumTrackTintColor={c.primary}
                maximumTrackTintColor={c.border}
                thumbTintColor={c.primary}
                onValueChange={handleValueChange}
                onSlidingComplete={handleComplete}
            />

            <ThemedText type="caption" style={{ color: c.textSecondary }}>
                {spec.help}
            </ThemedText>

            {liveNote ? (
                <ThemedText
                    type="caption"
                    style={{ color: c.info }}
                    testID={`param-live-${spec.key}`}
                >
                    {liveNote}
                </ThemedText>
            ) : null}

            {clampNote ? (
                <ThemedText
                    type="caption"
                    style={{ color: c.warning }}
                    testID={`param-clamp-${spec.key}`}
                >
                    {clampNote}
                </ThemedText>
            ) : null}
        </View>
    );
}

export const ParamSliderRow = memo(
    ParamSliderRowImpl,
    (prev, next) =>
        prev.spec === next.spec &&
        prev.value === next.value &&
        prev.busy === next.busy &&
        prev.clampNote === next.clampNote &&
        prev.liveNote === next.liveNote &&
        prev.showFirmwareLabel === next.showFirmwareLabel &&
        prev.disabled === next.disabled &&
        prev.onSlide === next.onSlide &&
        prev.onSlideComplete === next.onSlideComplete &&
        prev.onHelp === next.onHelp,
);

const styles = StyleSheet.create({
    // 60 dp of vertical room per control: this is used one-handed, in the dark, by someone who
    // is not looking at the phone as carefully as they would indoors.
    wrap: { paddingVertical: Spacing.md, gap: Spacing.xs },
    header: { flexDirection: "row", alignItems: "center", justifyContent: "space-between", gap: Spacing.sm },
    labelWrap: { flexShrink: 1, gap: 1 },
    friendly: { fontSize: 15, fontWeight: "600" },
    readoutWrap: { flexDirection: "row", alignItems: "center", gap: Spacing.sm },
    readout: { fontSize: 15, fontWeight: "600", fontVariant: ["tabular-nums"] },
    busyDot: { width: 6, height: 6, borderRadius: 3 },
    helpButton: {
        width: 22,
        height: 22,
        borderRadius: Radii.pill,
        borderWidth: 1,
        alignItems: "center",
        justifyContent: "center",
    },
    slider: { width: "100%", height: 40 },
});
