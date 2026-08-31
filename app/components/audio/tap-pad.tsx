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
  /**
   * The MINIMUM number of taps that can be fitted against — not a goal to stop at.
   *
   * It used to be a target the step auto-completed on, so the pad read "12 / 24" and stopping
   * was the app's decision. Collection is open-ended now: tapping past the minimum only makes
   * the fit better, and a pad that read "15 / 8" would look broken at exactly the moment the
   * user was doing the right thing.
   */
  minimum: number;
  disabled?: boolean;
  onTap: () => void;
  testID?: string;
}

export const TapPad = memo(function TapPad({
  count,
  minimum,
  disabled,
  onTap,
  testID = "tap-pad",
}: Props) {
  const colors = useThemeColors();
  const enough = count >= minimum;

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
      accessibilityLabel={
        enough
          ? `Tap along with the beat. ${count} taps recorded, enough to fit. Keep going for a better fit.`
          : `Tap along with the beat. ${count} of ${minimum} taps recorded, ${minimum - count} more needed.`
      }
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
        {enough ? `${count} taps` : `${count} / ${minimum}`}
      </ThemedText>
      <ThemedText type="caption" style={{ color: colors.textSecondary }}>
        {enough ? "Enough to fit — more is better" : "Tap on every beat"}
      </ThemedText>
      <View style={styles.dots}>
        {Array.from({ length: 8 }, (_, i) => (
          <View
            key={i}
            style={[
              styles.dot,
              {
                backgroundColor:
                  enough || i < Math.min(8, Math.round((count / minimum) * 8))
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
