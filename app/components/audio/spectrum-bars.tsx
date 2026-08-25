import { memo } from "react";
import { StyleSheet, View } from "react-native";
import Animated, { useAnimatedStyle } from "react-native-reanimated";

import { Radii } from "@/constants/theme";
import { useAudioTelemetry } from "@/context/audio-telemetry-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { AUDIO_NUM_DISPLAY_BUCKETS } from "@/services/audio-telemetry";

/**
 * Twenty log-scaled buckets. The fastest way to spot a dead mic or a phone in a pocket —
 * both of which look identical to "the music is quiet" on a level meter alone.
 *
 * Only present at tier 3 (48 bytes), so it is the first thing to disappear on a degraded
 * link. The caller renders a notice in that case rather than showing twenty empty bars,
 * which would read as silence rather than as missing data.
 */

interface Props {
  live: boolean;
  testID?: string;
}

export const SpectrumBars = memo(function SpectrumBars({
  live,
  testID = "spectrum-bars",
}: Props) {
  const colors = useThemeColors();
  return (
    <View
      style={styles.row}
      testID={testID}
      accessible
      accessibilityLabel={live ? "Spectrum, 20 bands" : "Spectrum unavailable"}
    >
      {Array.from({ length: AUDIO_NUM_DISPLAY_BUCKETS }, (_, i) => (
        <Bucket
          key={i}
          index={i}
          live={live}
          color={colors.primary}
          track={colors.surfaceAlt}
        />
      ))}
    </View>
  );
});

function Bucket({
  index,
  live,
  color,
  track,
}: {
  index: number;
  live: boolean;
  color: string;
  track: string;
}) {
  const telemetry = useAudioTelemetry();
  const style = useAnimatedStyle(() => {
    const v = telemetry ? telemetry.shared.buckets[index].value : 0;
    return { height: `${Math.max(2, Math.min(v, 1) * 100)}%` };
  });
  return (
    <View style={[styles.slot, { backgroundColor: track }]}>
      <Animated.View
        style={[
          styles.bar,
          { backgroundColor: color, opacity: live ? 1 : 0.4 },
          style,
        ]}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  row: { flexDirection: "row", alignItems: "flex-end", height: 56, gap: 2 },
  slot: {
    flex: 1,
    height: "100%",
    borderRadius: Radii.sm,
    overflow: "hidden",
    justifyContent: "flex-end",
  },
  bar: { width: "100%", borderRadius: Radii.sm },
});
