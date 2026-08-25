import { memo } from "react";
import { StyleSheet, View } from "react-native";
import Animated, { useAnimatedStyle } from "react-native-reanimated";

import { ThemedText } from "@/components/themed-text";
import { Radii, Spacing } from "@/constants/theme";
import { useAudioTelemetry } from "@/context/audio-telemetry-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { AUDIO_NUM_BANDS } from "@/services/audio-telemetry";

/**
 * Four bars answering the one question the serial shell could never answer at a venue:
 * "why isn't it firing?"
 *
 * Each bar is flux / threshold, with a FIXED TICK AT 1.0 labelled "fires here". Seeing a bar
 * sit steadily at 0.7 tells you both that it is close and how much sensitivity you need —
 * which is strictly more information than a beat indicator that simply stays dark.
 *
 * The bar is a ratio, not a recomputed decision. Both numbers are quantised to 0.5 dB on the
 * wire, so `ratio > 1` here is NOT the detector's verdict and must never be drawn as one; the
 * authoritative beat bit rides separately and drives BeatPulse.
 */

export const BAND_LABELS = ["Kick", "Low-mid", "Mid", "High"] as const;
export const BAND_RANGES = [
  "31–200 Hz",
  "219–781 Hz",
  "813–1969 Hz",
  "2.0–6.0 kHz",
] as const;

/** Matches BAND_RATIO_MAX in the provider: the bar is full at 1.5x the fire line. */
const RATIO_MAX = 1.5;

interface Props {
  /** Latest ratios for the accessibility label; the bars themselves animate off shared values. */
  ratios: number[];
  live: boolean;
  testID?: string;
}

export const BandHeadroomBars = memo(function BandHeadroomBars({
  ratios,
  live,
  testID = "band-headroom-bars",
}: Props) {
  const colors = useThemeColors();

  return (
    <View testID={testID}>
      {Array.from({ length: AUDIO_NUM_BANDS }, (_, b) => (
        <BandRow
          key={b}
          band={b}
          ratio={ratios[b] ?? 0}
          live={live}
          colors={colors}
          testID={`${testID}-${b}`}
        />
      ))}
      <ThemedText
        type="caption"
        style={{ color: colors.textMuted, marginTop: Spacing.xs }}
      >
        The line is where a beat fires. A bar that never reaches it needs more
        Sensitivity.
      </ThemedText>
    </View>
  );
});

function BandRow({
  band,
  ratio,
  live,
  colors,
  testID,
}: {
  band: number;
  ratio: number;
  live: boolean;
  colors: ReturnType<typeof useThemeColors>;
  testID: string;
}) {
  const telemetry = useAudioTelemetry();

  const fillStyle = useAnimatedStyle(() => {
    const r = telemetry ? telemetry.shared.bandRatio[band].value : 0;
    const pct = Math.min(r / RATIO_MAX, 1) * 100;
    return { width: `${pct}%` };
  });

  const pctOfFire = Math.round(ratio * 100);
  return (
    <View
      style={styles.row}
      testID={testID}
      accessible
      accessibilityLabel={
        live
          ? `${BAND_LABELS[band]}, ${BAND_RANGES[band]}, at ${pctOfFire} percent of the firing level`
          : `${BAND_LABELS[band]}, no signal`
      }
    >
      <ThemedText
        type="caption"
        style={[styles.label, { color: colors.textSecondary }]}
      >
        {BAND_LABELS[band]}
      </ThemedText>
      <View style={[styles.track, { backgroundColor: colors.surfaceAlt }]}>
        <Animated.View
          style={[
            styles.fill,
            { backgroundColor: colors.primary, opacity: live ? 1 : 0.4 },
            fillStyle,
          ]}
        />
        {/* The fire line. Fixed, so bars are comparable across bands and over time. */}
        <View
          style={[
            styles.fireTick,
            { left: `${(1 / RATIO_MAX) * 100}%`, backgroundColor: colors.text },
          ]}
        />
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  row: { flexDirection: "row", alignItems: "center", marginBottom: Spacing.xs },
  label: { width: 62 },
  track: { flex: 1, height: 14, borderRadius: Radii.sm, overflow: "hidden" },
  fill: { position: "absolute", left: 0, top: 0, bottom: 0 },
  fireTick: { position: "absolute", top: 0, bottom: 0, width: 2, opacity: 0.8 },
});
