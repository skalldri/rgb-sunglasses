import { memo } from "react";
import { StyleSheet, View } from "react-native";

import { ThemedText } from "@/components/themed-text";
import { Radii, Spacing } from "@/constants/theme";
import { useThemeColors } from "@/hooks/use-theme-color";
import type { Verdict, VerdictTone } from "@/services/audio-scoreboard";

/**
 * One sentence, one fix. See computeVerdict() for why exactly one is shown.
 *
 * Tone drives a colour AND an always-present text label, never colour alone: this is read in
 * a dark room, at a glance, possibly by someone colour-blind.
 */

interface Props {
  verdict: Verdict;
  testID?: string;
}

export const VerdictBanner = memo(function VerdictBanner({
  verdict,
  testID = "verdict-banner",
}: Props) {
  const colors = useThemeColors();
  const toneColor: Record<VerdictTone, string> = {
    neutral: colors.textMuted,
    good: colors.success,
    warning: colors.warning,
    bad: colors.danger,
  };
  const accent = toneColor[verdict.tone];

  return (
    <View
      testID={testID}
      accessible
      accessibilityLiveRegion="polite"
      accessibilityLabel={`${verdict.title}. ${verdict.detail}`}
      style={[
        styles.box,
        { borderColor: accent, backgroundColor: colors.surfaceAlt },
      ]}
    >
      <View style={[styles.stripe, { backgroundColor: accent }]} />
      <View style={styles.text}>
        <ThemedText
          type="defaultSemiBold"
          style={{ color: accent }}
          testID={`${testID}-title`}
        >
          {verdict.title}
        </ThemedText>
        <ThemedText
          type="caption"
          style={{ color: colors.textSecondary }}
          testID={`${testID}-detail`}
        >
          {verdict.detail}
        </ThemedText>
      </View>
    </View>
  );
});

const styles = StyleSheet.create({
  box: {
    flexDirection: "row",
    borderWidth: StyleSheet.hairlineWidth,
    borderRadius: Radii.md,
    overflow: "hidden",
    minHeight: 56,
    alignItems: "center",
  },
  stripe: { width: 4, alignSelf: "stretch" },
  text: {
    flex: 1,
    paddingVertical: Spacing.sm,
    paddingHorizontal: Spacing.md,
    gap: 2,
  },
});
