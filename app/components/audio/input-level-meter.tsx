import { memo } from "react";
import { StyleSheet, View } from "react-native";
import Animated, { useAnimatedStyle } from "react-native-reanimated";

import { ThemedText } from "@/components/themed-text";
import { Radii, Spacing } from "@/constants/theme";
import { useAudioTelemetry } from "@/context/audio-telemetry-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { TELEMETRY_DB_FLOOR, magnitudeToDb } from "@/services/audio-telemetry";

/**
 * The whole AGC story in one widget: where the sound is, where we want it, and where it
 * stops being listened to.
 *
 * Reading it answers the three questions someone at a venue actually has — "below the window,
 * so it is turning up", "in the window, so it is done", "below the gate, so it has stopped
 * listening" — and the fix for each is to drag something on this same widget.
 *
 * Drawn as plain Views with a Reanimated width, not a chart library: this is one bar, and
 * useAnimatedStyle mutates it on the UI thread with no React involvement at all. A 32 Hz
 * stream costs zero renders here.
 */

/** Meter track spans -80..0 dBFS. Below -80 there is nothing actionable to see. */
const METER_MIN_DB = -80;
const METER_MAX_DB = 0;

function pctFromDb(db: number): number {
  const clamped =
    db < METER_MIN_DB ? METER_MIN_DB : db > METER_MAX_DB ? METER_MAX_DB : db;
  return ((clamped - METER_MIN_DB) / (METER_MAX_DB - METER_MIN_DB)) * 100;
}

export interface InputLevelMeterProps {
  /** AGC target window, as raw normalised RMS (the firmware's own units). */
  targetLow: number | null;
  targetHigh: number | null;
  /** Noise gate, raw normalised RMS. Zero means the gate is off. */
  noiseGate: number | null;
  /** Latest values, for the accessibility label and the frozen fallback. */
  rmsInputDb: number;
  gainDb: number;
  headroomDb: number;
  live: boolean;
  testID?: string;
}

export const InputLevelMeter = memo(function InputLevelMeter({
  targetLow,
  targetHigh,
  noiseGate,
  rmsInputDb,
  gainDb,
  headroomDb,
  live,
  testID = "input-level-meter",
}: InputLevelMeterProps) {
  const colors = useThemeColors();
  const telemetry = useAudioTelemetry();

  const fillStyle = useAnimatedStyle(() => {
    const db = telemetry
      ? telemetry.shared.rmsInputDb.value
      : TELEMETRY_DB_FLOOR;
    return { width: `${pctFromDb(db)}%` };
  });

  const peakStyle = useAnimatedStyle(() => {
    const db = telemetry ? telemetry.shared.peakDb.value : TELEMETRY_DB_FLOOR;
    return { left: `${pctFromDb(db)}%` };
  });

  const lowPct =
    targetLow !== null ? pctFromDb(magnitudeToDb(targetLow)) : null;
  const highPct =
    targetHigh !== null ? pctFromDb(magnitudeToDb(targetHigh)) : null;
  const gatePct =
    noiseGate && noiseGate > 0 ? pctFromDb(magnitudeToDb(noiseGate)) : null;

  const gainText = `${gainDb >= 0 ? "+" : ""}${gainDb.toFixed(1)} dB`;

  /* Reanimated is mocked under jest, so the visual state is unassertable in a unit test. The
   * numbers therefore live in the accessibility label, which is both the only testable
   * surface AND a genuine win for anyone using a screen reader in a dark room. */
  const a11y = live
    ? `Input level ${Math.round(rmsInputDb)} decibels, mic gain ${gainText}, ` +
      `${Math.round(headroomDb)} decibels of headroom`
    : "Input level unavailable, no signal from the glasses";

  return (
    <View testID={testID} accessible accessibilityLabel={a11y}>
      <View
        style={[
          styles.track,
          { backgroundColor: colors.surfaceAlt, borderColor: colors.border },
        ]}
      >
        {/* Sweet spot: where the AGC is trying to keep the music. */}
        {lowPct !== null && highPct !== null ? (
          <View
            testID={`${testID}-target`}
            style={[
              styles.zone,
              {
                left: `${lowPct}%`,
                width: `${Math.max(highPct - lowPct, 0)}%`,
                backgroundColor: colors.success,
                opacity: 0.22,
              },
            ]}
          />
        ) : null}
        {/* Gate zone: below this the glasses stop reacting entirely. */}
        {gatePct !== null ? (
          <View
            testID={`${testID}-gate`}
            style={[
              styles.zone,
              {
                left: 0,
                width: `${gatePct}%`,
                backgroundColor: colors.textMuted,
                opacity: 0.3,
              },
            ]}
          />
        ) : null}
        {/* Clip zone: the top 3 dB. */}
        <View
          style={[
            styles.zone,
            {
              left: `${pctFromDb(-3)}%`,
              right: 0,
              backgroundColor: colors.danger,
              opacity: 0.25,
            },
          ]}
        />
        <Animated.View
          testID={`${testID}-fill`}
          style={[
            styles.fill,
            { backgroundColor: colors.primary, opacity: live ? 1 : 0.4 },
            fillStyle,
          ]}
        />
        <Animated.View
          testID={`${testID}-peak`}
          style={[
            styles.peak,
            { backgroundColor: colors.text, opacity: live ? 0.9 : 0.35 },
            peakStyle,
          ]}
        />
      </View>
      <View style={styles.legend}>
        <ThemedText type="caption" style={{ color: colors.textMuted }}>
          {METER_MIN_DB} dBFS
        </ThemedText>
        <ThemedText
          type="caption"
          style={{ color: colors.textSecondary }}
          testID={`${testID}-gain`}
        >
          mic gain {gainText} · {Math.round(headroomDb)} dB headroom
        </ThemedText>
        <ThemedText type="caption" style={{ color: colors.textMuted }}>
          0
        </ThemedText>
      </View>
    </View>
  );
});

const styles = StyleSheet.create({
  track: {
    height: 28,
    borderRadius: Radii.sm,
    borderWidth: StyleSheet.hairlineWidth,
    overflow: "hidden",
    justifyContent: "center",
  },
  zone: { position: "absolute", top: 0, bottom: 0 },
  fill: { position: "absolute", left: 0, top: 0, bottom: 0 },
  peak: { position: "absolute", top: 0, bottom: 0, width: 2 },
  legend: {
    flexDirection: "row",
    justifyContent: "space-between",
    alignItems: "center",
    marginTop: Spacing.xs,
  },
});
