import React from "react";

import {
  TELEMETRY_TIER_STATS,
  extractCalibrationWindow,
  type TelemetryRing,
} from "@/services/audio-telemetry";
import {
  FRAME_MS,
  analyzeMusic,
  analyzeRoom,
  assessTaps,
  classifyTempo,
  matchTaps,
  reconcileGate,
  replayBeats,
  sweepSensitivity,
  type CalibrationWindow,
  type ProposedChange,
} from "@/services/audio-calibration";
import {
  AUDIO_PARAMS,
  alphaFromSensitivity,
  deltaFromSensitivity,
  type AudioParamKey,
} from "@/services/audio-params";

/**
 * Step machine for the guided calibration wizard.
 *
 * IT WRITES NOTHING UNTIL THE FINAL REVIEW. Every step only records and analyses; the caller
 * applies the proposal explicitly, per row, after seeing a diff. That is not politeness — a
 * wizard that writes as it goes leaves the device in a half-calibrated state if the user
 * cancels or walks out of range mid-flow, which at a venue is the likeliest outcome.
 */

export const STEP_SECONDS = { room: 8, music: 15, tap: 30 } as const;
/** Enough taps to fit to; the step ends early here rather than making the user tap for 30 s. */
export const TAP_TARGET = 24;

export type WizardStep =
  | "intro"
  | "room"
  | "music"
  | "tap"
  | "review"
  | "applying"
  | "done"
  | "failed";

export type StepOutcome = { ok: true } | { ok: false; reason: string };

export type CalibrationState = {
  step: WizardStep;
  /** Whole seconds remaining in a recording step. */
  secondsLeft: number;
  tapCount: number;
  /** Populated once the review step is reached. */
  changes: ProposedChange[];
  warnings: string[];
  notes: string[];
  /** Set when a step refused; the caller shows this and offers a retry. */
  failure: string | null;
  applyProgress: { done: number; total: number } | null;
  applyError: string | null;
};

type Deps = {
  ring: React.MutableRefObject<TelemetryRing>;
  requestStream: (tier: 1 | 2 | 3 | null, rateHz?: number) => void;
  /** Current device value for a parameter, or null if absent. */
  valueOf: (key: AudioParamKey) => number | null;
  /** Writes one parameter. Resolves false (does not throw) on a rejected write. */
  writeParam: (key: AudioParamKey, value: number) => Promise<boolean>;
  /**
   * Snapshot the current settings before applying, so there is always a way back.
   * Returns false when nothing could be saved — the caller must not promise a rescue that
   * does not exist.
   */
  snapshotPreset: (name: string) => boolean;
  now?: () => number;
};

/** Sensitivity 1..10, mapped through the same macro the Simple tab uses. */
function sensitivityCandidates(thresholdMode: 0 | 1) {
  return Array.from({ length: 10 }, (_, i) => ({
    sensitivity: i + 1,
    paramValue:
      thresholdMode === 1
        ? deltaFromSensitivity(i + 1)
        : alphaFromSensitivity(i + 1),
  }));
}

export function useAudioCalibration(deps: Deps) {
  const depsRef = React.useRef(deps);
  depsRef.current = deps;

  /* Stable identity, routed through depsRef like every other dep in this file.
   *
   * `deps.now ?? (() => Date.now())` minted a fresh closure on every render, and `now` sits
   * in the dep array of captureWindow -> beginStep -> finishTap/finishMusic/finishRoom ->
   * start/recordTap, so all seven useCallbacks rebuilt every render and TapPad's memo (which
   * receives recordTap) never hit — the memoization was pure overhead. Cheap today because
   * the countdown only re-renders ~1/s, but any future effect keyed on these callbacks would
   * re-fire on every render. */
  const now = React.useCallback(() => depsRef.current.now?.() ?? Date.now(), []);

  const [state, setState] = React.useState<CalibrationState>({
    step: "intro",
    secondsLeft: 0,
    tapCount: 0,
    changes: [],
    warnings: [],
    notes: [],
    failure: null,
    applyProgress: null,
    applyError: null,
  });

  /* Analysis results carried between steps. Refs, not state: nothing renders from them until
   * the review step, and putting them in state would re-render the recording UI each tick. */
  const roomRef = React.useRef<{
    roomP95: number;
    proposedFloor: number;
    /** Loud room: measured and fitted, but a level gate cannot clear the background. */
    noisy: boolean;
    warnings: string[];
  } | null>(null);
  const musicRef = React.useRef<{
    targetLow: number;
    targetHigh: number;
    musicP5: number;
  } | null>(null);
  const tapsRef = React.useRef<number[]>([]);
  const windowStartRef = React.useRef(0);
  const timerRef = React.useRef<ReturnType<typeof setInterval> | null>(null);
  /* Bumped by cancel() and by unmount. The apply loop captures it and stops on any change:
   * Cancel stays live during "applying", and without this the user could walk away at
   * "Applying 2 of 5..." while the device kept being rewritten behind them — precisely the
   * half-applied state this batch design exists to prevent, since depsRef keeps writeParam
   * callable after unmount. */
  const abortRef = React.useRef(0);

  const clearTimer = React.useCallback(() => {
    if (timerRef.current) {
      clearInterval(timerRef.current);
      timerRef.current = null;
    }
  }, []);

  React.useEffect(
    () => () => {
      abortRef.current++;
      clearTimer();
    },
    [clearTimer],
  );

  const captureWindow = React.useCallback((): CalibrationWindow & {
    medianStepMs: number;
  } => {
    return extractCalibrationWindow(
      depsRef.current.ring.current,
      windowStartRef.current,
      now(),
    );
  }, [now]);

  /** Begin a timed recording step. */
  const beginStep = React.useCallback(
    (step: "room" | "music" | "tap", onDone: () => void) => {
      clearTimer();
      tapsRef.current = step === "tap" ? [] : tapsRef.current;
      windowStartRef.current = now();
      const seconds = STEP_SECONDS[step];
      setState((s) => ({
        ...s,
        step,
        secondsLeft: seconds,
        failure: null,
        tapCount: 0,
      }));

      const endAt = now() + seconds * 1000;
      timerRef.current = setInterval(() => {
        const left = Math.max(0, Math.ceil((endAt - now()) / 1000));
        setState((s) =>
          s.secondsLeft === left ? s : { ...s, secondsLeft: left },
        );
        if (left <= 0) {
          clearTimer();
          onDone();
        }
      }, 250);
    },
    [clearTimer, now],
  );

  const fail = React.useCallback(
    (reason: string) => {
      clearTimer();
      /* Restore the default stream request. Without this, requestRef stays at the tap step's
       * {tier 2, 32 Hz} after a failure, so the retry's identical requestStream() call hits
       * the no-edge early return and skips the ring reset that "start the recording clean"
       * depends on — the retry then records into a window still holding the failed run. */
      depsRef.current.requestStream(null);
      setState((s) => ({ ...s, step: "failed", failure: reason }));
    },
    [clearTimer],
  );

  /* ── step 3/4 boundary: everything that needs both windows ── */
  const finishTap = React.useCallback(() => {
    clearTimer();
    const d = depsRef.current;
    const win = captureWindow();
    const taps = tapsRef.current;
    const room = roomRef.current;
    const music = musicRef.current;
    if (!room || !music) {
      fail("Something went wrong partway through. Start again.");
      return;
    }

    const changes: ProposedChange[] = [];
    const warnings: string[] = [];
    const notes: string[] = [];

    const push = (key: AudioParamKey, newValue: number, because: string) => {
      const oldValue = d.valueOf(key);
      if (oldValue === null) return;
      /* Skip no-op rows: a diff table full of unchanged values buries the real changes. */
      if (Math.abs(oldValue - newValue) < 1e-9) return;
      changes.push({
        key,
        label: AUDIO_PARAMS[key].friendlyLabel,
        oldValue,
        newValue,
        because,
      });
    };

    /* The room's own findings first: they frame every row below them, and in a loud room they
     * are the difference between "these numbers look odd" and "this is what this room costs". */
    warnings.push(...room.warnings);

    push(
      "beatFluxFloor",
      room.proposedFloor,
      room.noisy
        ? "Measured from the room's background noise."
        : "Measured from the quiet room.",
    );
    push(
      "agcTargetLow",
      music.targetLow,
      "Fitted to the quiet parts of the music.",
    );
    push(
      "agcTargetHigh",
      music.targetHigh,
      "Fitted to the loud parts of the music.",
    );

    const gate = reconcileGate(room.roomP95, music.musicP5);
    push(
      "agcNoiseGateRms",
      gate.noiseGate,
      "Set between the room noise and the quiet music.",
    );
    if (gate.warning) warnings.push(gate.warning);

    /* ── the tap-derived half, which is allowed to contribute nothing ── */
    const quality = assessTaps(taps);
    if (!quality.ok) {
      notes.push(quality.reason);
    } else if (!win.hasStats) {
      notes.push(
        "This link could not send the detail needed to try other sensitivities, so that has been left alone.",
      );
    } else if (win.medianStepMs > FRAME_MS * 1.5) {
      /* The refractory is counted in frames. Fitting one against a decimated window would
       * model a longer refractory than the device applies, so refuse rather than mislead. */
      notes.push(
        "The glasses could not send frames fast enough to fit the sensitivity, so that has been left alone.",
      );
    } else {
      const mode = win.thresholdMode;
      const sensitivityKey: AudioParamKey =
        mode === 1 ? "beatSfDelta" : "beatAlpha";
      const floor = room.proposedFloor;
      const candidates = sensitivityCandidates(mode);
      const sweep = sweepSensitivity(win, taps, candidates, floor);

      if (!sweep.ok) {
        notes.push(sweep.reason);
      } else {
        /* Report the improvement honestly, against the settings actually in use. */
        const currentValue = d.valueOf(sensitivityKey);
        const currentRefractory = d.valueOf("beatRefractoryFrames") ?? 5;
        const before =
          currentValue !== null
            ? matchTaps(
                taps,
                replayBeats(win, currentValue, currentRefractory, floor),
              ).f
            : 0;

        push(
          sensitivityKey,
          sweep.best.paramValue,
          `Matched ${Math.round(sweep.best.f * 100)}% of your taps, up from ${Math.round(before * 100)}%.`,
        );
        push(
          "beatRefractoryFrames",
          sweep.best.refractoryFrames,
          "Chosen alongside the sensitivity.",
        );

        /* The two classic failures, named explicitly. Both look like "it's wrong" but need
         * opposite fixes, and saying which is happening saves a lot of guessing. */
        const detected = replayBeats(
          win,
          sweep.best.paramValue,
          sweep.best.refractoryFrames,
          floor,
        );
        const spanS =
          detected.length > 1
            ? (detected[detected.length - 1] - detected[0]) / 1000
            : 0;
        const detectedBpm =
          spanS > 0 ? ((detected.length - 1) / spanS) * 60 : 0;
        /* The two classic failures, REPORTED but not silently corrected.
         *
         * The copy used to claim a correction had been applied ("the gap between beats has
         * been lengthened to suit"). Nothing applied one: the pushed rows are the sweep's own
         * best F-score, chosen BEFORE this classification and never amended. And because the
         * classification runs on a replay of that same winner, a double/half result is proof
         * the APPLIED settings still show the failure — so the claim was affirmatively false
         * exactly when it appeared. The user applied, believed the double-fire was fixed, and
         * the lights still strobed twice per beat.
         *
         * Saying what was observed, and what to do about it, is honest and still useful:
         * these two failures need opposite fixes, so naming which one is happening is most of
         * the value. */
        const relation = classifyTempo(detectedBpm, quality.bpm);
        if (relation.kind === "double") {
          const ms = Math.round(relation.proposedRefractoryFrames * FRAME_MS);
          notes.push(
            `${relation.message} I could not fix that automatically — try Beat feel "Kick only", or a gap of about ${ms} ms in Advanced.`,
          );
        } else if (relation.kind === "half") {
          notes.push(
            `${relation.message} I could not fix that automatically — try turning Sensitivity up a step by hand.`,
          );
        }
      }
    }

    if (changes.length === 0) {
      fail("Everything already looks right for this room. Nothing to change.");
      return;
    }

    d.requestStream(null); // back to the screen's default rate
    setState((s) => ({
      ...s,
      step: "review",
      changes,
      warnings,
      notes,
      secondsLeft: 0,
    }));
  }, [captureWindow, clearTimer, fail]);

  const finishMusic = React.useCallback(() => {
    const win = captureWindow();
    const result = analyzeMusic(win);
    if (!result.ok) {
      fail(result.reason);
      return;
    }
    musicRef.current = {
      targetLow: result.targetLow,
      targetHigh: result.targetHigh,
      musicP5: result.musicP5,
    };
    /* The tap step needs undecimated tier-2 frames — see requestStream's comment. */
    depsRef.current.requestStream(TELEMETRY_TIER_STATS as 2, 32);
    beginStep("tap", finishTap);
  }, [beginStep, captureWindow, fail, finishTap]);

  const finishRoom = React.useCallback(() => {
    const win = captureWindow();
    const result = analyzeRoom(win);
    if (!result.ok) {
      fail(result.reason);
      return;
    }
    roomRef.current = {
      roomP95: result.roomP95,
      proposedFloor: result.proposedFloor,
      noisy: result.noisy,
      warnings: result.warnings,
    };
    beginStep("music", finishMusic);
  }, [beginStep, captureWindow, fail, finishMusic]);

  const start = React.useCallback(() => {
    roomRef.current = null;
    musicRef.current = null;
    tapsRef.current = [];
    setState({
      step: "intro",
      secondsLeft: 0,
      tapCount: 0,
      changes: [],
      warnings: [],
      notes: [],
      failure: null,
      applyProgress: null,
      applyError: null,
    });
    beginStep("room", finishRoom);
  }, [beginStep, finishRoom]);

  const recordTap = React.useCallback(() => {
    tapsRef.current.push(now());
    const count = tapsRef.current.length;
    setState((s) => ({ ...s, tapCount: count }));
    if (count >= TAP_TARGET) {
      finishTap();
    }
  }, [finishTap, now]);

  const cancel = React.useCallback(() => {
    abortRef.current++;
    clearTimer();
    depsRef.current.requestStream(null);
    setState((s) => ({ ...s, step: "intro", secondsLeft: 0, failure: null }));
  }, [clearTimer]);

  /**
   * Apply the accepted rows.
   *
   * Sequential, because Android permits one outstanding GATT operation. The pre-change
   * snapshot is saved FIRST and unconditionally, so a failure partway through still leaves a
   * named way back to where the device was.
   */
  const apply = React.useCallback(async (accepted: ProposedChange[]) => {
    const d = depsRef.current;
    if (accepted.length === 0) {
      setState((s) => ({ ...s, step: "done" }));
      return;
    }
    /* If the snapshot did not land there is no way back, so say so up front rather than
     * discovering it only if a write later fails. Applying anyway is still the user's call —
     * the wizard's whole job is to change these values — but the promise has to be honest. */
    const rescued = d.snapshotPreset("Before calibration");
    setState((s) => ({
      ...s,
      step: "applying",
      applyProgress: { done: 0, total: accepted.length },
      applyError: null,
    }));

    const abortGeneration = abortRef.current;
    for (let i = 0; i < accepted.length; i++) {
      if (abortRef.current !== abortGeneration) {
        /* Cancelled or unmounted mid-batch. Stop where we are and say so rather than
         * silently finishing — and never flip to "done", which on the disconnect path could
         * otherwise report success after the user had already bailed. */
        setState((s) => ({
          ...s,
          step: "review",
          applyProgress: null,
          applyError:
            rescued
              ? 'Stopped partway through. Your previous settings are saved as "Before calibration".'
              : "Stopped partway through, and your previous settings could NOT be saved.",
        }));
        return;
      }
      const change = accepted[i];
      let ok = false;
      try {
        ok = await d.writeParam(change.key as AudioParamKey, change.newValue);
      } catch {
        ok = false;
      }
      if (!ok) {
        /* Stop at the first failure rather than pressing on. A partially-applied fit is
         * worse than none: the parameters interact, and half of them is a combination
         * nothing measured. The snapshot taken above is the way back. */
        setState((s) => ({
          ...s,
          step: "review",
          applyProgress: null,
          applyError: rescued
            ? `Could not set ${change.label}. Nothing after it was changed; your previous settings are saved as "Before calibration".`
            : `Could not set ${change.label}. Nothing after it was changed — and your previous settings could NOT be saved, so note them before trying again.`,
        }));
        return;
      }
      setState((s) => ({
        ...s,
        applyProgress: { done: i + 1, total: accepted.length },
      }));
    }
    setState((s) => ({ ...s, step: "done", applyProgress: null }));
  }, []);

  return { state, start, recordTap, cancel, apply };
}
