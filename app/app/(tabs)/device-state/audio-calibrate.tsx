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
  type CollectorKind,
  type CalibrationState,
} from "@/hooks/use-audio-calibration";
import { useAudioParams } from "@/hooks/use-audio-params";
import { useAudioPresets } from "@/hooks/use-audio-presets";
import { useThemeColors } from "@/hooks/use-theme-color";
import {
  AUDIO_PARAMS,
  formatParamValue,
  type AudioParamKey,
} from "@/services/audio-params";
import { MIN_TAPS, type ProposedChange } from "@/services/audio-calibration";

/**
 * "Tune it for me" — opportunistic calibration.
 *
 * A COLLECTION BOARD, NOT A WIZARD. There is no countdown and no required order. Each
 * collector is toggled on around whatever moment the venue happens to offer and accumulates
 * across as many sittings as it takes; Fit runs when the operator says so.
 *
 * The countdown version this replaced was unusable anywhere it mattered: it demanded 8 s of
 * silence, then 15 s of music, then 30 s of tapping, each starting the instant the app decided.
 * A venue does not take direction — the band starts when it starts — and the flow did not fail
 * loudly when it was ignored, it fitted whatever happened to be playing and reported success.
 *
 * Still writes NOTHING until the final review, and still auto-saves the pre-change settings as
 * a named preset before the first write.
 */

/* No provider here: the device-state stack layout owns the single AudioTelemetryProvider. */
export default function AudioCalibrateScreen() {
  const c = useThemeColors();
  const router = useRouter();
  const { selectedDevice } = useBluetooth();
  const telemetry = useAudioTelemetry();

  /* All parameter plumbing comes from ONE hook, shared with the tuning screen. */
  const { resolved, currentValues, valueOf, writeParam } = useAudioParams();

  const presets = useAudioPresets({ currentValues, writeParam });

  /**
   * Save the pre-change settings under EXACTLY the name the UI promises.
   *
   * The return value is checked rather than discarded: saveCurrentAs yields null when no values
   * resolved, and promising a way back that was never written is worse than saying so.
   */
  const snapshotPreset = useCallback(
    (name: string) => presets.saveCurrentAs(name, Date.now()) !== null,
    [presets],
  );

  const {
    state,
    startCollecting,
    stopCollecting,
    discardLast,
    recordTap,
    fit,
    reset,
    cancel,
    apply,
  } = useAudioCalibration({
    ring: telemetry?.ring ?? { current: null as never },
    requestStream: telemetry?.requestStream ?? (() => {}),
    valueOf,
    writeParam,
    snapshotPreset,
  });

  /* Per-row accept toggles. Default on: the user asked to be tuned, so opting OUT of a single
   * row is the exception. */
  const [rejected, setRejected] = useState<Set<string>>(new Set());

  /* Clear rejections whenever a fresh proposal is produced. The hook resets its own state, but
   * this set lives on a screen that stays mounted across runs: rows toggled off in run 1 would
   * otherwise arrive already-excluded in run 2, worst case an "Apply 0 changes" button,
   * disabled, with nothing on screen explaining why. */
  const runFit = useCallback(() => {
    setRejected(new Set());
    fit();
  }, [fit]);
  const startOver = useCallback(() => {
    setRejected(new Set());
    reset();
  }, [reset]);
  /* Back to the board WITHOUT throwing anything away. A fit can fail for reasons that have
   * nothing to do with what was collected ("nothing to change"), and a venue may not offer
   * those quiet moments again. */
  const backToCollecting = useCallback(() => {
    setRejected(new Set());
    cancel();
  }, [cancel]);
  const accepted = state.changes.filter((ch) => !rejected.has(ch.key));

  /* THE FLOW IS ENTIRELY TELEMETRY-DEPENDENT, so it gates on telemetry — not on the audio
   * CONFIG service. Those are separate axes: firmware can expose the tunables without the
   * telemetry stream, and in that case every collector would sit at 0 frames forever with the
   * copy blaming the room for a firmware capability gap. */
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

  const collecting = state.phase === "collecting";

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
              The guided tune-up listens to what the glasses are hearing, and
              this firmware cannot send that yet. You can still tune everything
              by hand on the previous screen.
            </ThemedText>
            <AppButton
              testID="calibrate-unsupported-back"
              onPress={() => router.back()}
              title="Back to tuning"
              style={styles.primaryButton}
            />
          </Card>
        ) : null}

        {!telemetryUnsupported && collecting ? (
          <>
            <Card>
              <ThemedText type="defaultSemiBold">
                Collect whenever it suits
              </ThemedText>
              <ThemedText type="caption" style={{ color: c.textSecondary }}>
                Nothing is timed and nothing has to be done in order. Start a
                collector when the moment is right, stop it when it stops being
                right, and come back to it as often as you like.
              </ThemedText>
              <ThemedText
                type="caption"
                style={{ color: c.textMuted, marginTop: Spacing.sm }}
              >
                Nothing is changed until you have seen exactly what I want to
                change.
              </ThemedText>
            </Card>

            <CollectorCard
              kind="background"
              title="Background noise"
              hint="Whatever the room does with the music off. A noisy room is fine — crowd, air handling, chatter. It gets measured and fitted to, not rejected."
              activeHint="Listening to the room. Stop if the music starts."
              state={state}
              onStart={startCollecting}
              onStop={stopCollecting}
              onDiscard={discardLast}
            />

            <CollectorCard
              kind="music"
              title="Music"
              hint="A representative stretch of what will actually be played. Collect more than one if the set changes character."
              activeHint="Listening to the music."
              state={state}
              onStart={startCollecting}
              onStop={stopCollecting}
              onDiscard={discardLast}
            />

            <CollectorCard
              kind="taps"
              title="Tap along"
              hint="Optional. Tap the beat and I can fit Sensitivity to what you hear; skip it and everything else is still fitted."
              activeHint="Tap the pad on every beat you hear."
              state={state}
              onStart={startCollecting}
              onStop={stopCollecting}
              onDiscard={discardLast}
            >
              {state.active === "taps" ? (
                <>
                  <TapPad
                    count={state.tapCount}
                    minimum={MIN_TAPS}
                    onTap={recordTap}
                    testID="tap-pad"
                  />
                  <ThemedText
                    type="caption"
                    style={{ color: c.textMuted }}
                    testID="calibrate-tap-count"
                  >
                    {state.tapCount} tap{state.tapCount === 1 ? "" : "s"}
                  </ThemedText>
                </>
              ) : null}
            </CollectorCard>

            <Card>
              <AppButton
                testID="calibrate-fit"
                disabled={!state.canFit}
                onPress={runFit}
                title="Work out my settings"
                style={styles.primaryButton}
              />
              <ThemedText
                type="caption"
                style={{ color: c.textMuted }}
                testID="calibrate-fit-hint"
              >
                {state.canFit
                  ? state.collectors.taps.ready
                    ? "Ready. Everything will be fitted, including Sensitivity."
                    : "Ready. Without taps, everything except Sensitivity will be fitted."
                  : "Needs background noise and music before it can work anything out."}
              </ThemedText>
            </Card>
          </>
        ) : null}

        {state.phase === "review" || state.phase === "applying" ? (
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
                disabled={state.phase === "applying"}
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
              disabled={state.phase === "applying" || accepted.length === 0}
              onPress={() => apply(accepted)}
              style={styles.primaryButton}
              title={
                state.phase === "applying" && state.applyProgress
                  ? `Applying ${state.applyProgress.done} of ${state.applyProgress.total}...`
                  : `Apply ${accepted.length} change${accepted.length === 1 ? "" : "s"}`
              }
            />
            {state.phase === "review" ? (
              <AppButton
                testID="calibrate-collect-more"
                onPress={startOver}
                title="Collect more instead"
                style={styles.secondaryButton}
              />
            ) : null}
          </Card>
        ) : null}

        {state.phase === "failed" ? (
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
            {/* Back to the board, NOT a fresh start: what was collected is still good, and a
                venue may not offer those moments again. */}
            <AppButton
              testID="calibrate-retry"
              onPress={backToCollecting}
              title="Back to collecting"
              style={styles.primaryButton}
            />
            <AppButton
              testID="calibrate-start-over"
              onPress={startOver}
              title="Throw it away and start over"
              style={styles.secondaryButton}
            />
          </Card>
        ) : null}

        {state.phase === "done" ? (
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
            while collecting taps it is how they confirm the glasses hear what they hear. */}
        <MonitorPanel
          targetLow={valueOf("agcTargetLow")}
          targetHigh={valueOf("agcTargetHigh")}
          noiseGate={valueOf("agcNoiseGateRms")}
        />
      </ScrollView>
    </SafeAreaView>
  );
}

/**
 * One collector: how much it holds, a start/stop toggle, and a way to throw away the last
 * sitting.
 *
 * The readout counts SECONDS COLLECTED, never elapsed time, and does not cap at the minimum —
 * topping up past "ready" improves the fit, and a collector that refused more would make a
 * marginal sample permanent.
 */
function CollectorCard({
  kind,
  title,
  hint,
  activeHint,
  state,
  onStart,
  onStop,
  onDiscard,
  children,
}: {
  kind: CollectorKind;
  title: string;
  hint: string;
  activeHint: string;
  state: CalibrationState;
  onStart: (k: CollectorKind) => void;
  onStop: () => void;
  onDiscard: (k: CollectorKind) => void;
  children?: React.ReactNode;
}) {
  const c = useThemeColors();
  const r = state.collectors[kind];
  const isActive = state.active === kind;
  /* Another collector is running. Starting this one would stop that one mid-sitting, which is
   * never what someone means to do. */
  const otherActive = state.active !== null && !isActive;

  return (
    <Card>
      <View style={styles.collectorHead}>
        <View style={styles.collectorTitle}>
          <ThemedText type="defaultSemiBold">{title}</ThemedText>
          <ThemedText
            type="caption"
            style={{ color: r.ready ? c.success : c.textSecondary }}
            testID={`calibrate-readout-${kind}`}
          >
            {r.ready ? "✓ " : ""}
            {r.seconds.toFixed(1)}s collected
            {r.chunks > 0
              ? ` · ${r.chunks} go${r.chunks === 1 ? "" : "es"}`
              : ""}
          </ThemedText>
        </View>
      </View>

      <ProgressBar
        progress={Math.min(1, r.needFrames > 0 ? r.frames / r.needFrames : 0)}
      />

      <ThemedText type="caption" style={{ color: c.textMuted }}>
        {isActive ? activeHint : hint}
      </ThemedText>

      {children}

      <AppButton
        testID={`calibrate-toggle-${kind}`}
        disabled={otherActive}
        onPress={() => (isActive ? onStop() : onStart(kind))}
        title={isActive ? "Stop" : r.chunks > 0 ? "Collect more" : "Start"}
        style={styles.primaryButton}
      />
      {r.chunks > 0 && !isActive ? (
        <AppButton
          testID={`calibrate-discard-${kind}`}
          onPress={() => onDiscard(kind)}
          title="Discard the last one"
          style={styles.secondaryButton}
        />
      ) : null}
    </Card>
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
  secondaryButton: {
    minHeight: 48,
    borderRadius: Radii.md,
    alignItems: "center",
    justifyContent: "center",
    marginTop: Spacing.sm,
  },
  collectorHead: { flexDirection: "row", alignItems: "center" },
  collectorTitle: { flex: 1, minWidth: 0, gap: 2 },
  changeRow: {
    flexDirection: "row",
    alignItems: "center",
    gap: Spacing.md,
    paddingVertical: Spacing.sm,
  },
  changeText: { flex: 1, gap: 2 },
});
