import { useRouter } from "expo-router";
import React, { useCallback, useMemo, useState } from "react";
import { Pressable, ScrollView, StyleSheet, Switch, View } from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";

import { MonitorPanel } from "@/components/audio/monitor-panel";
import { TapPad } from "@/components/audio/tap-pad";
import { ThemedText } from "@/components/themed-text";
import { Card } from "@/components/ui/card";
import { EmptyState } from "@/components/ui/empty-state";
import { UUID_AUDIO_CONFIG_SERVICE } from "@/constants/bluetooth";
import { Radii, Spacing } from "@/constants/theme";
import {
  AudioTelemetryProvider,
  useAudioTelemetry,
} from "@/context/audio-telemetry-context";
import { useBluetooth } from "@/context/bluetooth-context";
import {
  useAudioCalibration,
  STEP_SECONDS,
  TAP_TARGET,
} from "@/hooks/use-audio-calibration";
import { useAudioParamWriter } from "@/hooks/use-audio-param-writer";
import { useAudioPresets } from "@/hooks/use-audio-presets";
import { useThemeColors } from "@/hooks/use-theme-color";
import {
  AUDIO_PARAMS,
  encodeParam,
  formatParamValue,
  resolveAudioParams,
  type AudioParamKey,
} from "@/services/audio-params";
import type { ProposedChange } from "@/services/audio-calibration";

/**
 * "Tune it for me" — the guided calibration wizard.
 *
 * A pushed full-screen route rather than a modal, with Cancel always visible, because it holds
 * the user's attention for ~30 s in a loud room and needs an unambiguous way out at every
 * point. It writes NOTHING until the final review, and auto-saves the pre-change settings as
 * a named preset before the first write, so there is always a way back.
 */

export default function AudioCalibrateRoute() {
  return (
    <AudioTelemetryProvider>
      <AudioCalibrateScreen />
    </AudioTelemetryProvider>
  );
}

function AudioCalibrateScreen() {
  const c = useThemeColors();
  const router = useRouter();
  const { selectedDevice, writeServiceCharacteristic } = useBluetooth();
  const telemetry = useAudioTelemetry();

  const serviceChars =
    selectedDevice?.characteristicsByService?.[UUID_AUDIO_CONFIG_SERVICE];
  const resolved = useMemo(
    () => resolveAudioParams(serviceChars ?? {}),
    [serviceChars],
  );

  const write = useCallback(
    (uuid: string, encoded: string) =>
      writeServiceCharacteristic(UUID_AUDIO_CONFIG_SERVICE, uuid, encoded),
    [writeServiceCharacteristic],
  );
  const writer = useAudioParamWriter(useMemo(() => ({ write }), [write]));

  const byKey = useMemo(() => {
    const map = {} as Partial<Record<AudioParamKey, (typeof resolved)[number]>>;
    resolved.forEach((r) => {
      map[r.spec.key] = r;
    });
    return map;
  }, [resolved]);

  const valueOf = useCallback(
    (key: AudioParamKey): number | null => {
      const entry = byKey[key];
      if (!entry) return null;
      return writer.displayValue(entry.spec.uuid, entry.value);
    },
    [byKey, writer],
  );

  const writeParam = useCallback(
    (key: AudioParamKey, value: number) => {
      const spec = AUDIO_PARAMS[key];
      return writer.writeNow(spec.uuid, value, (v) => encodeParam(spec, v));
    },
    [writer],
  );

  const currentValues = useMemo(() => {
    const out: Partial<Record<AudioParamKey, number>> = {};
    resolved.forEach((r) => {
      const v = writer.displayValue(r.spec.uuid, r.value);
      if (v !== null) out[r.spec.key] = v;
    });
    return out;
  }, [resolved, writer]);

  const presets = useAudioPresets({ currentValues, writeParam });

  const snapshotPreset = useCallback(
    (name: string) => {
      /* Named with the time, so a second run does not silently overwrite the first run's
       * escape hatch — saveCurrentAs overwrites by name. */
      const stamp = new Date().toLocaleTimeString([], {
        hour: "2-digit",
        minute: "2-digit",
      });
      presets.saveCurrentAs(`${name} · ${stamp}`, Date.now());
    },
    [presets],
  );

  const { state, start, recordTap, cancel, apply } = useAudioCalibration({
    ring: telemetry?.ring ?? { current: null as never },
    requestStream: telemetry?.requestStream ?? (() => {}),
    valueOf,
    writeParam,
    snapshotPreset,
  });

  /* Per-row accept toggles. Default on: the user asked to be tuned, so opting OUT of a single
   * row is the exception. Keyed by param key, so a re-run resets cleanly. */
  const [rejected, setRejected] = useState<Set<string>>(new Set());
  const accepted = state.changes.filter((ch) => !rejected.has(ch.key));

  if (!selectedDevice || resolved.length === 0) {
    return (
      <SafeAreaView
        style={[styles.screen, { backgroundColor: c.background }]}
        edges={["top"]}
      >
        <EmptyState
          title="Not connected"
          subtitle="Connect to your glasses to run the calibration."
        />
      </SafeAreaView>
    );
  }

  return (
    <SafeAreaView
      style={[styles.screen, { backgroundColor: c.background }]}
      edges={["top"]}
    >
      <View style={styles.header}>
        <ThemedText type="heading">Tune it for me</ThemedText>
        <Pressable
          testID="calibrate-cancel"
          onPress={() => {
            cancel();
            router.back();
          }}
          hitSlop={12}
          accessibilityRole="button"
        >
          <ThemedText style={{ color: c.primary }}>Cancel</ThemedText>
        </Pressable>
      </View>

      <ScrollView contentContainerStyle={styles.content}>
        {state.step === "intro" ? (
          <Card>
            <ThemedText type="defaultSemiBold">
              This takes about a minute
            </ThemedText>
            <ThemedText type="caption" style={{ color: c.textSecondary }}>
              First I listen to the empty room, then to the music, then you tap
              along with the beat. Nothing is changed until you have seen
              exactly what I want to change.
            </ThemedText>
            <ThemedText
              type="caption"
              style={{ color: c.textMuted, marginTop: Spacing.sm }}
            >
              Start the first step between songs, while the room is quiet.
            </ThemedText>
            <Pressable
              testID="calibrate-start"
              onPress={start}
              style={[styles.primaryButton, { backgroundColor: c.primary }]}
              accessibilityRole="button"
            >
              <ThemedText style={{ color: c.onPrimary }}>Start</ThemedText>
            </Pressable>
          </Card>
        ) : null}

        {state.step === "room" || state.step === "music" ? (
          <Card>
            <ThemedText type="defaultSemiBold" testID="calibrate-step-title">
              {state.step === "room"
                ? "Listening to the room"
                : "Listening to the music"}
            </ThemedText>
            <ThemedText type="caption" style={{ color: c.textSecondary }}>
              {state.step === "room"
                ? "Keep it as quiet as you can — this measures the background noise."
                : "Let a normal, representative track play."}
            </ThemedText>
            <ThemedText type="heading" testID="calibrate-countdown">
              {state.secondsLeft}s
            </ThemedText>
            <ProgressBar
              value={1 - state.secondsLeft / STEP_SECONDS[state.step]}
              color={c.primary}
              track={c.surfaceAlt}
            />
          </Card>
        ) : null}

        {state.step === "tap" ? (
          <Card>
            <ThemedText type="defaultSemiBold">Tap along</ThemedText>
            <ThemedText type="caption" style={{ color: c.textSecondary }}>
              Tap the pad on every beat you hear. This is the only way to check
              the glasses are finding the same beats you are.
            </ThemedText>
            <TapPad
              count={state.tapCount}
              target={TAP_TARGET}
              onTap={recordTap}
            />
          </Card>
        ) : null}

        {state.step === "review" || state.step === "applying" ? (
          <Card>
            <ThemedText type="defaultSemiBold">
              Here is what I want to change
            </ThemedText>
            {state.applyError ? (
              <ThemedText
                type="caption"
                style={{ color: c.danger }}
                testID="calibrate-apply-error"
              >
                {state.applyError}
              </ThemedText>
            ) : null}
            {state.changes.map((ch) => (
              <ChangeRow
                key={ch.key}
                change={ch}
                accepted={!rejected.has(ch.key)}
                disabled={state.step === "applying"}
                onToggle={() =>
                  setRejected((prev) => {
                    const next = new Set(prev);
                    if (next.has(ch.key)) next.delete(ch.key);
                    else next.add(ch.key);
                    return next;
                  })
                }
              />
            ))}
            {state.warnings.map((w, i) => (
              <ThemedText
                key={`w${i}`}
                type="caption"
                style={{ color: c.warning }}
                testID={`calibrate-warning-${i}`}
              >
                {w}
              </ThemedText>
            ))}
            {state.notes.map((n, i) => (
              <ThemedText
                key={`n${i}`}
                type="caption"
                style={{ color: c.textMuted }}
                testID={`calibrate-note-${i}`}
              >
                {n}
              </ThemedText>
            ))}
            <ThemedText type="caption" style={{ color: c.textMuted }}>
              Your current settings will be saved as &quot;Before
              calibration&quot; first.
            </ThemedText>
            <Pressable
              testID="calibrate-apply"
              disabled={state.step === "applying" || accepted.length === 0}
              onPress={() => apply(accepted)}
              style={[
                styles.primaryButton,
                {
                  backgroundColor:
                    accepted.length === 0 ? c.surfaceAlt : c.primary,
                  opacity: state.step === "applying" ? 0.6 : 1,
                },
              ]}
              accessibilityRole="button"
            >
              <ThemedText
                style={{
                  color: accepted.length === 0 ? c.textMuted : c.onPrimary,
                }}
              >
                {state.step === "applying" && state.applyProgress
                  ? `Applying ${state.applyProgress.done} of ${state.applyProgress.total}...`
                  : `Apply ${accepted.length} change${accepted.length === 1 ? "" : "s"}`}
              </ThemedText>
            </Pressable>
          </Card>
        ) : null}

        {state.step === "failed" ? (
          <Card>
            <ThemedText type="defaultSemiBold" style={{ color: c.warning }}>
              Could not finish
            </ThemedText>
            <ThemedText
              type="caption"
              style={{ color: c.textSecondary }}
              testID="calibrate-failure"
            >
              {state.failure}
            </ThemedText>
            <Pressable
              testID="calibrate-retry"
              onPress={start}
              style={[styles.primaryButton, { backgroundColor: c.primary }]}
              accessibilityRole="button"
            >
              <ThemedText style={{ color: c.onPrimary }}>Try again</ThemedText>
            </Pressable>
          </Card>
        ) : null}

        {state.step === "done" ? (
          <Card>
            <ThemedText type="defaultSemiBold" style={{ color: c.success }}>
              Done
            </ThemedText>
            <ThemedText type="caption" style={{ color: c.textSecondary }}>
              Watch the meters for a moment. If it is not right, your old
              settings are saved as &quot;Before calibration&quot;.
            </ThemedText>
            <Pressable
              testID="calibrate-finish"
              onPress={() => router.back()}
              style={[styles.primaryButton, { backgroundColor: c.primary }]}
              accessibilityRole="button"
            >
              <ThemedText style={{ color: c.onPrimary }}>
                Back to tuning
              </ThemedText>
            </Pressable>
          </Card>
        ) : null}

        {/* The monitor stays visible throughout: it is how the user sees the room respond, and
            during the tap step it is how they confirm the glasses hear what they hear. */}
        <MonitorPanel
          targetLow={valueOf("agcTargetLow")}
          targetHigh={valueOf("agcTargetHigh")}
          noiseGate={valueOf("agcNoiseGateRms")}
        />
      </ScrollView>
    </SafeAreaView>
  );
}

function ProgressBar({
  value,
  color,
  track,
}: {
  value: number;
  color: string;
  track: string;
}) {
  const pct = Math.max(0, Math.min(1, value)) * 100;
  return (
    <View style={[styles.progressTrack, { backgroundColor: track }]}>
      <View
        style={[
          styles.progressFill,
          { width: `${pct}%`, backgroundColor: color },
        ]}
      />
    </View>
  );
}

function ChangeRow({
  change,
  accepted,
  disabled,
  onToggle,
}: {
  change: ProposedChange;
  accepted: boolean;
  disabled: boolean;
  onToggle: () => void;
}) {
  const c = useThemeColors();
  const spec = AUDIO_PARAMS[change.key as AudioParamKey];
  return (
    <View style={styles.changeRow} testID={`calibrate-change-${change.key}`}>
      <View style={styles.changeText}>
        <ThemedText type="defaultSemiBold">{change.label}</ThemedText>
        <ThemedText type="caption" style={{ color: c.textSecondary }}>
          {formatParamValue(spec, change.oldValue)} →{" "}
          {formatParamValue(spec, change.newValue)}
        </ThemedText>
        <ThemedText type="caption" style={{ color: c.textMuted }}>
          {change.because}
        </ThemedText>
      </View>
      <Switch
        testID={`calibrate-accept-${change.key}`}
        value={accepted}
        onValueChange={onToggle}
        disabled={disabled}
        accessibilityLabel={`Apply ${change.label}`}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1 },
  header: {
    flexDirection: "row",
    alignItems: "center",
    justifyContent: "space-between",
    paddingHorizontal: Spacing.lg,
    paddingVertical: Spacing.md,
  },
  content: { padding: Spacing.lg, gap: Spacing.lg, paddingBottom: Spacing.xxl },
  primaryButton: {
    minHeight: 56,
    borderRadius: Radii.md,
    alignItems: "center",
    justifyContent: "center",
    marginTop: Spacing.md,
  },
  progressTrack: {
    height: 8,
    borderRadius: Radii.sm,
    overflow: "hidden",
    marginTop: Spacing.sm,
  },
  progressFill: { height: "100%" },
  changeRow: {
    flexDirection: "row",
    alignItems: "center",
    gap: Spacing.md,
    paddingVertical: Spacing.sm,
  },
  changeText: { flex: 1, gap: 2 },
});
