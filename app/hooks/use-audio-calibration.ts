import React from "react";

import {
  TELEMETRY_TIER_STATS,
  extractCalibrationWindow,
  type TelemetryRing,
} from "@/services/audio-telemetry";
import {
  FRAME_MS,
  MIN_MUSIC_FRAMES,
  MIN_ROOM_FRAMES,
  analyzeMusic,
  analyzeRoom,
  assessTaps,
  classifyTempo,
  matchTaps,
  reconcileGate,
  replayBeats,
  sweepSensitivity,
  type ProposedChange,
} from "@/services/audio-calibration";
import {
  appendChunk,
  dropLastChunk,
  emptyPool,
  readiness,
  sealPool,
  type CalibrationPool,
  type PoolReadiness,
} from "@/services/audio-calibration-pool";
import {
  AUDIO_PARAMS,
  alphaFromSensitivity,
  deltaFromSensitivity,
  type AudioParamKey,
} from "@/services/audio-params";

/**
 * Opportunistic calibration: three collectors that accumulate, and a fit you run when ready.
 *
 * THIS REPLACED A TIMER-DRIVEN STEP MACHINE, and the reason is the whole design.
 *
 * The old flow recorded 8 s of room, then 15 s of music, then 30 s of tapping, in that order,
 * each starting the moment the app decided. That works in a living room and nowhere else. At a
 * venue the music starts and stops on the band's direction; a quiet moment lasts as long as it
 * lasts; and nobody is going to hold the room silent because a phone started a countdown. A
 * step that demands conditions on cue does not fail loudly when it does not get them — it fits
 * whatever was playing and reports success. Caught exactly that way in testing: the room step
 * measured crowd noise correctly, the music step then expired before music could be started,
 * and the flow marched on to tapping with a music window full of crowd.
 *
 * So nothing here has a duration. Each collector is toggled on and off by the operator around
 * whatever moment the venue happens to offer, accumulates across as many sittings as it takes,
 * and reports how close it is to being usable. Ordering is free. A sitting ruined halfway
 * through (the band came back early) is discarded on its own and costs nothing else.
 *
 * IT STILL WRITES NOTHING UNTIL THE FINAL REVIEW. The apply path below is carried over
 * unchanged from the step machine, because that part was never the problem.
 */

/** Ring is circular, so a live collector must drain into its pool faster than wraparound. */
const DRAIN_MS = 1000;

export type CollectorKind = "background" | "music" | "taps";

/**
 * Frames each collector needs before it is usable.
 *
 * Background and music reuse the analysis layer's own minimums, so the readiness pill and the
 * refusal inside analyzeRoom/analyzeMusic can never disagree about what "enough" means. Taps
 * need frames for the sweep to replay over AND enough taps to fit against; MIN_TAPS lives in
 * the analysis layer and is checked there.
 */
export const COLLECTOR_MIN_FRAMES: Record<CollectorKind, number> = {
  background: MIN_ROOM_FRAMES,
  music: MIN_MUSIC_FRAMES,
  taps: MIN_ROOM_FRAMES,
};

export type CalibrationPhase =
  | "collecting"
  | "review"
  | "applying"
  | "done"
  | "failed";

export type CalibrationState = {
  phase: CalibrationPhase;
  /** Which collector is recording right now, or null. Only ever one at a time. */
  active: CollectorKind | null;
  collectors: Record<CollectorKind, PoolReadiness>;
  tapCount: number;
  /** Background and music are gathered; the fit can run. Taps are optional. */
  canFit: boolean;
  changes: ProposedChange[];
  warnings: string[];
  notes: string[];
  failure: string | null;
  applyProgress: { done: number; total: number } | null;
  applyError: string | null;
};

type Deps = {
  ring: React.MutableRefObject<TelemetryRing>;
  requestStream: (tier: 1 | 2 | 3 | null, rateHz?: number) => void;
  valueOf: (key: AudioParamKey) => number | null;
  writeParam: (key: AudioParamKey, value: number) => Promise<boolean>;
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

function emptyPools(): Record<CollectorKind, CalibrationPool> {
  return { background: emptyPool(), music: emptyPool(), taps: emptyPool() };
}

export function useAudioCalibration(deps: Deps) {
  const depsRef = React.useRef(deps);
  depsRef.current = deps;

  /* Stable identity, routed through depsRef like every other dep here — a fresh closure per
   * render would rebuild every useCallback below and defeat TapPad's memo. */
  const now = React.useCallback(() => depsRef.current.now?.() ?? Date.now(), []);

  const poolsRef = React.useRef(emptyPools());
  const tapsRef = React.useRef<number[]>([]);
  /* The sitting in progress. Drained into on an interval, sealed into ONE chunk on stop, so
   * Discard removes a sitting rather than an arbitrary second of one. */
  const stagingRef = React.useRef<CalibrationPool>(emptyPool());
  const lastDrainRef = React.useRef(0);
  const activeRef = React.useRef<CollectorKind | null>(null);
  const timerRef = React.useRef<ReturnType<typeof setInterval> | null>(null);
  /* Bumped by cancel() and unmount; the apply loop stops on any change. depsRef keeps
   * writeParam callable after unmount, so without this the device could keep being rewritten
   * behind a user who walked away at "Applying 2 of 5". */
  const abortRef = React.useRef(0);

  const [state, setState] = React.useState<CalibrationState>(() => ({
    phase: "collecting",
    active: null,
    collectors: {
      background: readiness(emptyPool(), COLLECTOR_MIN_FRAMES.background),
      music: readiness(emptyPool(), COLLECTOR_MIN_FRAMES.music),
      taps: readiness(emptyPool(), COLLECTOR_MIN_FRAMES.taps),
    },
    tapCount: 0,
    canFit: false,
    changes: [],
    warnings: [],
    notes: [],
    failure: null,
    applyProgress: null,
    applyError: null,
  }));

  const publish = React.useCallback((activeNow: CollectorKind | null) => {
    const p = poolsRef.current;
    const collectors = {
      background: readiness(p.background, COLLECTOR_MIN_FRAMES.background),
      music: readiness(p.music, COLLECTOR_MIN_FRAMES.music),
      taps: readiness(p.taps, COLLECTOR_MIN_FRAMES.taps),
    };
    setState((s) => ({
      ...s,
      active: activeNow,
      collectors,
      tapCount: tapsRef.current.length,
      /* Taps are deliberately NOT required. The tap-derived half is allowed to contribute
       * nothing — fitting the gate and the AGC window without touching sensitivity is a
       * perfectly good outcome, and at a venue it may be all there is time for. */
      canFit: collectors.background.ready && collectors.music.ready,
    }));
  }, []);

  const clearTimer = React.useCallback(() => {
    if (timerRef.current) {
      clearInterval(timerRef.current);
      timerRef.current = null;
    }
  }, []);

  /** Pull everything the ring has gained since the last drain into the staging pool. */
  const drain = React.useCallback(() => {
    const t = now();
    const chunk = extractCalibrationWindow(
      depsRef.current.ring.current,
      lastDrainRef.current,
      t,
    );
    lastDrainRef.current = t;
    if (chunk.frames > 0) {
      stagingRef.current = appendChunk(stagingRef.current, chunk);
    }
  }, [now]);

  React.useEffect(
    () => () => {
      abortRef.current++;
      clearTimer();
    },
    [clearTimer],
  );

  const stopCollecting = React.useCallback(() => {
    if (!activeRef.current) return;
    clearTimer();
    drain();

    const kind = activeRef.current;
    const sealed = sealPool(stagingRef.current);
    poolsRef.current = {
      ...poolsRef.current,
      [kind]: appendChunk(poolsRef.current[kind], sealed),
    };
    stagingRef.current = emptyPool();
    activeRef.current = null;
    depsRef.current.requestStream(null);
    publish(null);
  }, [clearTimer, drain, publish]);

  const startCollecting = React.useCallback(
    (kind: CollectorKind) => {
      /* Only one at a time: two collectors sharing one stream would attribute the same frames
       * to both pools, and a background pool containing the music is the exact contamination
       * this design exists to make impossible. */
      if (activeRef.current) stopCollecting();

      /* Every collector needs tier 2. analyzeRoom fits the beat floor from band-0 FLUX, which
       * only exists at tier 2+, so even "just the room" cannot run on the meters-only tier.
       * Taps additionally need 32 Hz undecimated: the sweep fits a refractory counted in
       * FRAMES, and fitting one against decimated frames models a longer gap than the device
       * will actually apply. */
      if (kind === "taps") {
        depsRef.current.requestStream(TELEMETRY_TIER_STATS as 2, 32);
        tapsRef.current = [];
      } else {
        depsRef.current.requestStream(TELEMETRY_TIER_STATS as 2);
      }

      stagingRef.current = emptyPool();
      /* Start the window HERE, not at the ring's oldest frame: the ring is already full of
       * whatever the screen has been watching, and adopting it would fold minutes of unrelated
       * audio into the first sitting. */
      lastDrainRef.current = now();
      activeRef.current = kind;
      publish(kind);

      clearTimer();
      timerRef.current = setInterval(() => {
        drain();
        publish(activeRef.current);
      }, DRAIN_MS);
    },
    [clearTimer, drain, now, publish, stopCollecting],
  );

  /** Throw away the most recent sitting of one collector. */
  const discardLast = React.useCallback(
    (kind: CollectorKind) => {
      if (activeRef.current === kind) stopCollecting();
      poolsRef.current = {
        ...poolsRef.current,
        [kind]: dropLastChunk(poolsRef.current[kind]),
      };
      if (kind === "taps") tapsRef.current = [];
      publish(activeRef.current);
    },
    [publish, stopCollecting],
  );

  const recordTap = React.useCallback(() => {
    if (activeRef.current !== "taps") return;
    tapsRef.current.push(now());
    setState((s) => ({ ...s, tapCount: tapsRef.current.length }));
  }, [now]);

  const fail = React.useCallback((reason: string) => {
    depsRef.current.requestStream(null);
    setState((s) => ({ ...s, phase: "failed", failure: reason }));
  }, []);

  /**
   * Run the fit over whatever has been collected.
   *
   * Everything below this line is the step machine's own arithmetic, unchanged: the pools are
   * CalibrationWindows, so analyzeRoom/analyzeMusic/reconcileGate/sweepSensitivity never
   * learned that collection stopped being timed.
   */
  const fit = React.useCallback(() => {
    if (activeRef.current) stopCollecting();
    const d = depsRef.current;
    const pools = poolsRef.current;

    const roomResult = analyzeRoom(pools.background);
    if (!roomResult.ok) {
      fail(roomResult.reason);
      return;
    }
    const musicResult = analyzeMusic(pools.music);
    if (!musicResult.ok) {
      fail(musicResult.reason);
      return;
    }

    const changes: ProposedChange[] = [];
    const warnings: string[] = [];
    const notes: string[] = [];

    const push = (key: AudioParamKey, newValue: number, because: string) => {
      const oldValue = d.valueOf(key);
      if (oldValue === null) return;
      if (Math.abs(oldValue - newValue) < 1e-9) return;
      changes.push({
        key,
        label: AUDIO_PARAMS[key].friendlyLabel,
        oldValue,
        newValue,
        because,
      });
    };

    warnings.push(...roomResult.warnings);

    push(
      "beatFluxFloor",
      roomResult.proposedFloor,
      roomResult.noisy
        ? "Measured from the room's background noise."
        : "Measured from the quiet room.",
    );
    push(
      "agcTargetLow",
      musicResult.targetLow,
      "Fitted to the quiet parts of the music.",
    );
    push(
      "agcTargetHigh",
      musicResult.targetHigh,
      "Fitted to the loud parts of the music.",
    );

    const gate = reconcileGate(roomResult.roomP95, musicResult.musicP5);
    push(
      "agcNoiseGateRms",
      gate.noiseGate,
      "Set between the room noise and the quiet music.",
    );
    if (gate.warning) warnings.push(gate.warning);

    /* ── the tap-derived half, which is allowed to contribute nothing ── */
    const tapWin = pools.taps;
    const taps = tapsRef.current;
    const quality = assessTaps(taps);
    if (tapWin.frames === 0 || taps.length === 0) {
      notes.push(
        "You did not tap along, so Sensitivity has been left alone. You can collect taps and fit again at any time.",
      );
    } else if (!quality.ok) {
      notes.push(quality.reason);
    } else if (!tapWin.hasStats) {
      notes.push(
        "This link could not send the detail needed to try other sensitivities, so that has been left alone.",
      );
    } else if (tapWin.medianStepMs > FRAME_MS * 1.5) {
      notes.push(
        "The glasses could not send frames fast enough to fit the sensitivity, so that has been left alone.",
      );
    } else {
      const mode = tapWin.thresholdMode;
      const sensitivityKey: AudioParamKey =
        mode === 1 ? "beatSfDelta" : "beatAlpha";
      const floor = roomResult.proposedFloor;
      const sweep = sweepSensitivity(
        tapWin,
        taps,
        sensitivityCandidates(mode),
        floor,
      );

      if (!sweep.ok) {
        notes.push(sweep.reason);
      } else {
        const currentValue = d.valueOf(sensitivityKey);
        const currentRefractory = d.valueOf("beatRefractoryFrames") ?? 5;
        const before =
          currentValue !== null
            ? matchTaps(
                taps,
                replayBeats(tapWin, currentValue, currentRefractory, floor),
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

        /* The two classic failures, REPORTED but not silently corrected — the classification
         * runs on a replay of the sweep's own winner, so a double/half result is proof the
         * APPLIED settings still show it. Claiming a correction had been made was false
         * exactly when it appeared. */
        const detected = replayBeats(
          tapWin,
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

    d.requestStream(null);
    setState((s) => ({
      ...s,
      phase: "review",
      changes,
      warnings,
      notes,
      failure: null,
    }));
  }, [fail, stopCollecting]);

  /** Throw away every pool and start over. */
  const reset = React.useCallback(() => {
    if (activeRef.current) stopCollecting();
    poolsRef.current = emptyPools();
    tapsRef.current = [];
    stagingRef.current = emptyPool();
    setState((s) => ({
      ...s,
      phase: "collecting",
      changes: [],
      warnings: [],
      notes: [],
      failure: null,
      applyProgress: null,
      applyError: null,
    }));
    publish(null);
  }, [publish, stopCollecting]);

  const cancel = React.useCallback(() => {
    abortRef.current++;
    clearTimer();
    activeRef.current = null;
    depsRef.current.requestStream(null);
    setState((s) => ({ ...s, phase: "collecting", failure: null }));
  }, [clearTimer]);

  /**
   * Apply the accepted rows. Carried over unchanged from the step machine.
   *
   * Sequential, because Android permits one outstanding GATT operation. The pre-change
   * snapshot is saved FIRST and unconditionally, so a failure partway through still leaves a
   * named way back to where the device was.
   */
  const apply = React.useCallback(async (accepted: ProposedChange[]) => {
    const d = depsRef.current;
    if (accepted.length === 0) {
      setState((s) => ({ ...s, phase: "done" }));
      return;
    }
    const rescued = d.snapshotPreset("Before calibration");
    setState((s) => ({
      ...s,
      phase: "applying",
      applyProgress: { done: 0, total: accepted.length },
      applyError: null,
    }));

    const abortGeneration = abortRef.current;
    for (let i = 0; i < accepted.length; i++) {
      if (abortRef.current !== abortGeneration) {
        setState((s) => ({
          ...s,
          phase: "review",
          applyProgress: null,
          applyError: rescued
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
        /* Stop at the first failure rather than pressing on: the parameters interact, and half
         * of them is a combination nothing measured. */
        setState((s) => ({
          ...s,
          phase: "review",
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
    setState((s) => ({ ...s, phase: "done", applyProgress: null }));
  }, []);

  return {
    state,
    startCollecting,
    stopCollecting,
    discardLast,
    recordTap,
    fit,
    reset,
    cancel,
    apply,
  };
}
