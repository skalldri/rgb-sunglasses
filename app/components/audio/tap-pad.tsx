import { memo } from "react";
import { Pressable, StyleSheet, View } from "react-native";
import * as Haptics from "expo-haptics";

import { ThemedText } from "@/components/themed-text";
import { Radii, Spacing } from "@/constants/theme";
import { useThemeColors } from "@/hooks/use-theme-color";

/**
 * Tap along with the music.
 *
 * This is the only step that validates beat DETECTION rather than gain — every other
 * measurement in the wizard is about levels, and levels can look perfect while the detector
 * fires on hi-hats. It is also the only place a human tells the device what a beat is.
 *
 * Deliberately full-width and tall: it is used one-handed, in the dark, while looking at the
 * stage rather than the phone.
 */

interface Props {
  count: number;
  target: number;
  disabled?: boolean;
  onTap: () => void;
  testID?: string;
}

export const TapPad = memo(function TapPad({
  count,
  target,
  disabled,
  onTap,
  testID = "tap-pad",
}: Props) {
  const colors = useThemeColors();

  const handle = () => {
    if (disabled) return;
    /* Fire-and-forget: a haptics failure (unsupported device, permissions) must never cost a
     * tap, because the tap's TIMESTAMP is the measurement. */
    Haptics.impactAsync(Haptics.ImpactFeedbackStyle.Light).catch(() => {});
    onTap();
  };

  return (
    <Pressable
      testID={testID}
      onPress={handle}
      disabled={disabled}
      accessibilityRole="button"
      accessibilityLabel={`Tap along with the beat. ${count} of ${target} taps recorded.`}
      style={({ pressed }) => [
        styles.pad,
        {
          backgroundColor: pressed ? colors.primary : colors.surfaceAlt,
          borderColor: colors.primary,
          opacity: disabled ? 0.5 : 1,
        },
      ]}
    >
      <ThemedText type="heading" testID={`${testID}-count`}>
        {count} / {target}
      </ThemedText>
      <ThemedText type="caption" style={{ color: colors.textSecondary }}>
        Tap on every beat
      </ThemedText>
      <View style={styles.dots}>
        {Array.from({ length: 8 }, (_, i) => (
          <View
            key={i}
            style={[
              styles.dot,
              {
                backgroundColor:
                  i < Math.min(8, Math.round((count / target) * 8))
                    ? colors.primary
                    : colors.border,
              },
            ]}
          />
        ))}
      </View>
    </Pressable>
  );
});

const styles = StyleSheet.create({
  pad: {
    minHeight: 160,
    borderRadius: Radii.lg,
    borderWidth: 2,
    alignItems: "center",
    justifyContent: "center",
    gap: Spacing.sm,
  },
  dots: { flexDirection: "row", gap: Spacing.xs },
  dot: { width: 8, height: 8, borderRadius: 4 },
});
