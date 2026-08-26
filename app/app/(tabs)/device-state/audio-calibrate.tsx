import { useRouter } from "expo-router";
import React, { useCallback, useState } from "react";
import { Pressable, ScrollView, StyleSheet, Switch, View } from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";

import { MonitorPanel } from "@/components/audio/monitor-panel";
import { TapPad } from "@/components/audio/tap-pad";
import { ThemedText } from "@/components/themed-text";
import { AppButton } from "@/components/ui/app-button";
import { Card } from "@/components/ui/card";
import { ProgressBar } from "@/components/ui/progress-bar";
import { EmptyState } from "@/components/ui/empty-state";
import { Radii, Spacing } from "@/constants/theme";
import {
  useAudioTelemetry,
  useAudioTelemetryStatus,
} from "@/context/audio-telemetry-context";
import { useBluetooth } from "@/context/bluetooth-context";
import {
  useAudioCalibration,
  STEP_SECONDS,
  TAP_TARGET,
} from "@/hooks/use-audio-calibration";
import { useAudioParams } from "@/hooks/use-audio-params";
import { useAudioPresets } from "@/hooks/use-audio-presets";
import { useThemeColors } from "@/hooks/use-theme-color";
import {
  AUDIO_PARAMS,
  formatParamValue,
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

/* No provider here: the device-state stack layout owns the single AudioTelemetryProvider.
 * Mounting a second one on top of the still-mounted tuning screen's is what put two
 * subscriptions and two watchdogs on one characteristic. */
export default function AudioCalibrateScreen() {

  const c = useThemeColors();
  const router = useRouter();
  const { selectedDevice } = useBluetooth();
  const telemetry = useAudioTelemetry();

  /* All parameter plumbing comes from ONE hook, shared with the tuning screen. It used to be
   * ~60 lines copied between the two, and the copies had already diverged on how they
   * filtered a missing value. */
  const { resolved, currentValues, valueOf, writeParam } = useAudioParams();

  const presets = useAudioPresets({ currentValues, writeParam });

  /**
   * Save the pre-change settings under EXACTLY the name the UI promises.
   *
   * It used to append a timestamp ("Before calibration · 9:45 PM") while all three
   * user-facing strings said plainly "Before calibration" — so a user whose apply failed
   * mid-write went looking for a preset that did not exist under the promised name. The
   * timestamp was there to stop a second run overwriting the first run's escape hatch, but
   * that trade is backwards: the most recent pre-calibration state is the one worth keeping,
   * and an un-findable rescue preset is not a rescue.
   *
   * The return value is checked rather than discarded: saveCurrentAs yields null when no
   * values resolved, and promising a way back that was never written is worse than saying so.
   */
  const snapshotPreset = useCallback(
    (name: string) => presets.saveCurrentAs(name, Date.now()) !== null,
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

  /* Wrap start so BOTH entry points (Start and Try again) clear the per-row rejections. The
   * hook resets its own state, but this set lives on a screen that stays mounted across runs:
   * a user who toggled rows off in run 1 got run 2's fresh proposal rendered with those
   * switches already off and silently excluded — worst case an "Apply 0 changes" button,
   * disabled, with nothing on screen explaining why. */
  const startRun = useCallback(() => {
    setRejected(new Set());
    start();
  }, [start]);
  const accepted = state.changes.filter((ch) => !rejected.has(ch.key));

  /* THE WIZARD IS ENTIRELY TELEMETRY-DEPENDENT, so it must gate on telemetry — not, as it
   * did, on the audio CONFIG service. Those are separate axes: firmware can expose the
   * tunables without the telemetry stream (the app ships ahead of firmware by design). In
   * that case the user pressed Start, recorded an empty ring for 8 s, and was told "I did not
   * hear enough to measure the room. Try again." — forever, with the copy blaming the room
   * for a firmware capability gap. */
  const telemetryStatus = useAudioTelemetryStatus();
  const telemetryUnsupported = telemetryStatus === "unsupported";

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
        {telemetryUnsupported ? (
          <Card>
            <ThemedText type="defaultSemiBold">
              This needs newer firmware
            </ThemedText>
            <ThemedText type="caption" style={{ color: c.textSecondary }}>
              The guided tune-up listens to what the glasses are hearing, and this firmware
              cannot send that yet. You can still tune everything by hand on the previous
              screen.
            </ThemedText>
            <AppButton
              testID="calibrate-unsupported-back"
              onPress={() => router.back()}
              title="Back to tuning"
              style={styles.primaryButton}
            />
          </Card>
        ) : null}

        {!telemetryUnsupported && state.step === "intro" ? (
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
            <AppButton
              testID="calibrate-start"
              onPress={startRun}
              title="Start"
              style={styles.primaryButton}
            />
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
              progress={1 - state.secondsLeft / STEP_SECONDS[state.step]}
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
            <AppButton
              testID="calibrate-apply"
              disabled={state.step === "applying" || accepted.length === 0}
              onPress={() => apply(accepted)}
              style={styles.primaryButton}
              title={
                state.step === "applying" && state.applyProgress
                  ? `Applying ${state.applyProgress.done} of ${state.applyProgress.total}...`
                  : `Apply ${accepted.length} change${accepted.length === 1 ? "" : "s"}`
              }
            />
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
            <AppButton
              testID="calibrate-retry"
              onPress={startRun}
              title="Try again"
              style={styles.primaryButton}
            />
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
            <AppButton
              testID="calibrate-finish"
              onPress={() => router.back()}
              title="Back to tuning"
              style={styles.primaryButton}
            />
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
