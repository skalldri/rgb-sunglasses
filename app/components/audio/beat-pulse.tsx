import { memo } from "react";
import { StyleSheet, View } from "react-native";

import { ThemedText } from "@/components/themed-text";
import { Spacing } from "@/constants/theme";
import { useThemeColors } from "@/hooks/use-theme-color";
import { BAND_LABELS } from "@/components/audio/band-headroom-bars";

/**
 * Confidence at a glance: is it finding beats, roughly how fast, and which band.
 *
 * BPM is estimated in JS from the sticky-OR'd beat bits rather than on the device. That was a
 * deliberate firmware decision worth restating here, because it looks like an omission: a
 * credible on-device estimate needs an IOI histogram over several seconds of RAM and DSP
 * cycles, and it is tuning-dependent — a mis-tuned threshold would put a confident, wrong BPM
 * on the very screen whose job is fixing that tuning. Estimating here costs nothing and can
 * be improved without a firmware release.
 */

interface Props {
  bpm: number | null;
  beatsPerSecond: number;
  lastBeatBand: number | null;
  live: boolean;
  testID?: string;
}

export const BeatPulse = memo(function BeatPulse({
  bpm,
  beatsPerSecond,
  lastBeatBand,
  live,
  testID = "beat-pulse",
}: Props) {
  const colors = useThemeColors();
  const bandName = lastBeatBand !== null ? BAND_LABELS[lastBeatBand] : null;

  const a11y = !live
    ? "Beat detection: no signal"
    : bpm !== null
      ? `Beats: about ${bpm} BPM, ${beatsPerSecond.toFixed(1)} per second${bandName ? `, last on ${bandName}` : ""}`
      : `Beats: ${beatsPerSecond.toFixed(1)} per second, tempo unknown`;

  return (
    <View
      style={styles.row}
      testID={testID}
      accessible
      accessibilityLabel={a11y}
    >
      <View
        testID={`${testID}-dot`}
        style={[
          styles.dot,
          {
            backgroundColor:
              live && beatsPerSecond > 0 ? colors.primary : colors.surfaceAlt,
            borderColor: colors.border,
          },
        ]}
      />
      <View style={styles.text}>
        <ThemedText type="defaultSemiBold" testID={`${testID}-bpm`}>
          {live && bpm !== null ? `${bpm} BPM` : "— BPM"}
        </ThemedText>
        <ThemedText
          type="caption"
          style={{ color: colors.textSecondary }}
          testID={`${testID}-rate`}
        >
          {live ? `${beatsPerSecond.toFixed(1)} hits/s` : "no signal"}
          {bandName && live ? ` · ${bandName}` : ""}
        </ThemedText>
      </View>
    </View>
  );
});

const styles = StyleSheet.create({
  row: { flexDirection: "row", alignItems: "center", gap: Spacing.md },
  dot: {
    width: 44,
    height: 44,
    borderRadius: 22,
    borderWidth: StyleSheet.hairlineWidth,
  },
  text: { flex: 1 },
});
