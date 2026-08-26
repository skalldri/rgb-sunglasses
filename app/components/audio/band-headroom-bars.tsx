import { memo } from "react";
import { StyleSheet, View } from "react-native";
import Animated, { useAnimatedStyle } from "react-native-reanimated";

import { ThemedText } from "@/components/themed-text";
import { Radii, Spacing } from "@/constants/theme";
import { useAudioTelemetry } from "@/context/audio-telemetry-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { AUDIO_NUM_BANDS, BAND_RATIO_MAX } from "@/services/audio-telemetry";

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

/* The bar is full at BAND_RATIO_MAX x the fire line. Imported, not restated: this constant
 * also positions the fire tick and clamps what the provider writes, and a local copy meant
 * retuning one desynced the tick from where the bars actually saturate. */

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

  /* Capture the SHARED VALUE, never the context. A worklet closes over its whole reference
   * graph, and the context also holds the ring-buffer ref — which Reanimated then FREEZES,
   * so pushTelemetryBytes' `ring.count++` is silently refused and no frame ever lands. That
   * is not a crash: the stream runs, the device says "streaming", and the app sits on
   * NO SIGNAL forever. Hardware-found 2026-08-25. */
  const ratioValue = telemetry?.shared.bandRatio[band];

  const fillStyle = useAnimatedStyle(() => {
    const r = ratioValue ? ratioValue.value : 0;
    const pct = Math.min(r / BAND_RATIO_MAX, 1) * 100;
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
            {
              left: `${(1 / BAND_RATIO_MAX) * 100}%`,
              backgroundColor: colors.text,
            },
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
