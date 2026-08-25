/**
 * Tests for the calibration wizard's step machine.
 *
 * The properties worth pinning are the safety ones, because they are what stop the wizard
 * writing a confident wrong answer to a device someone is about to wear on stage: it writes
 * nothing before the review, it snapshots first, it stops at the first failed write, and every
 * step can refuse with a reason rather than inventing a number.
 */

import { act, renderHook } from "@testing-library/react-native";

import {
  useAudioCalibration,
  STEP_SECONDS,
  TAP_TARGET,
} from "@/hooks/use-audio-calibration";
import { AUDIO_PARAMS, type AudioParamKey } from "@/services/audio-params";
import {
  createTelemetryRing,
  pushTelemetryBytes,
  type TelemetryRing,
} from "@/services/audio-telemetry";
import { makeFrame } from "./fixtures/audio-telemetry";

/** Controllable clock so recording steps can be driven deterministically. */
function makeClock(start = 1_000_000) {
  let t = start;
  return {
    now: () => t,
    advance(ms: number) {
      t += ms;
    },
  };
}

type Harness = ReturnType<typeof makeHarness>;

function makeHarness(
  opts: { values?: Partial<Record<AudioParamKey, number>> } = {},
) {
  const clock = makeClock();
  const ring = { current: createTelemetryRing(1024) };
  const requestStream = jest.fn();
  const writeParam = jest.fn().mockResolvedValue(true);
  const snapshotPreset = jest.fn();
  const values: Partial<Record<AudioParamKey, number>> = {
    beatFluxFloor: AUDIO_PARAMS.beatFluxFloor.defaultValue,
    agcTargetLow: AUDIO_PARAMS.agcTargetLow.defaultValue,
    agcTargetHigh: AUDIO_PARAMS.agcTargetHigh.defaultValue,
    agcNoiseGateRms: AUDIO_PARAMS.agcNoiseGateRms.defaultValue,
    beatAlpha: AUDIO_PARAMS.beatAlpha.defaultValue,
    beatSfDelta: AUDIO_PARAMS.beatSfDelta.defaultValue,
    beatRefractoryFrames: AUDIO_PARAMS.beatRefractoryFrames.defaultValue,
    ...opts.values,
  };
  const valueOf = (k: AudioParamKey) => values[k] ?? null;

  return {
    clock,
    ring,
    requestStream,
    writeParam,
    snapshotPreset,
    valueOf,
    values,
  };
}

function renderCal(h: Harness) {
  return renderHook(() =>
    useAudioCalibration({
      ring: h.ring as unknown as { current: TelemetryRing },
      requestStream: h.requestStream,
      valueOf: h.valueOf,
      writeParam: h.writeParam,
      snapshotPreset: h.snapshotPreset,
      now: h.clock.now,
    }),
  );
}

/** Push `seconds` of frames into the ring, advancing the clock as it goes. */
function feed(
  h: Harness,
  seconds: number,
  frame: (n: number) => Uint8Array,
  rateHz = 31.25,
) {
  const stepMs = 1000 / rateHz;
  const total = Math.round(seconds * rateHz);
  for (let n = 0; n < total; n++) {
    pushTelemetryBytes(h.ring.current, frame(n), h.clock.now());
    h.clock.advance(stepMs);
  }
}

const QUIET_ROOM = (n: number) =>
  makeFrame({
    tier: 2,
    seq: n,
    rmsInput: 0.0005,
    flux: [0.01, 0, 0, 0],
    mean: [0.1, 0.1, 0.1, 0.1],
    sigma: [0.1, 0.1, 0.1, 0.1],
  });

const MUSIC = (n: number) =>
  makeFrame({
    tier: 2,
    seq: n,
    rmsInput: 0.01 + (n % 50) * 0.0008,
    flux: [n % 16 === 0 ? 0.3 : 0.17, 0, 0, 0],
    mean: [0.1, 0.1, 0.1, 0.1],
    sigma: [0.1, 0.1, 0.1, 0.1],
  });

/** Run the timer far enough that the step's interval fires and sees the deadline passed. */
function runStep(h: Harness, seconds: number) {
  act(() => {
    h.clock.advance(seconds * 1000);
    jest.advanceTimersByTime(seconds * 1000 + 500);
  });
}

describe("useAudioCalibration", () => {
  beforeEach(() => jest.useFakeTimers());
  afterEach(() => {
    // Deliberately NOT runOnlyPendingTimers: the library's auto-cleanup unmounts after this
    // hook, so draining here fires the step interval against a live component outside act().
    // The hook clears its own interval on unmount, so a pending timer simply never fires.
    jest.useRealTimers();
    jest.restoreAllMocks();
  });

  it("starts at the intro and writes nothing", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    expect(result.current.state.step).toBe("intro");
    expect(h.writeParam).not.toHaveBeenCalled();
    expect(h.snapshotPreset).not.toHaveBeenCalled();
  });

  it("counts down through the room step", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    act(() => result.current.start());
    expect(result.current.state.step).toBe("room");
    expect(result.current.state.secondsLeft).toBe(STEP_SECONDS.room);

    act(() => {
      h.clock.advance(3000);
      jest.advanceTimersByTime(3000);
    });
    expect(result.current.state.secondsLeft).toBe(STEP_SECONDS.room - 3);
  });

  it("refuses a room that is too loud, naming the reason", () => {
    // The most important refusal in the wizard: measuring a "noise floor" during the support
    // act produces a gate that mutes the headliner.
    const h = makeHarness();
    const { result } = renderCal(h);
    act(() => result.current.start());
    feed(h, STEP_SECONDS.room, (n) =>
      makeFrame({ tier: 2, seq: n, rmsInput: 0.02 }),
    );
    runStep(h, 0);

    expect(result.current.state.step).toBe("failed");
    expect(result.current.state.failure).toContain("not quiet enough");
    expect(h.writeParam).not.toHaveBeenCalled();
  });

  it("advances room -> music -> tap on good data", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    act(() => result.current.start());
    feed(h, STEP_SECONDS.room, QUIET_ROOM);
    runStep(h, 0);
    expect(result.current.state.step).toBe("music");

    feed(h, STEP_SECONDS.music, MUSIC);
    runStep(h, 0);
    expect(result.current.state.step).toBe("tap");
  });

  it("raises the stream to undecimated tier 2 for the tap step", () => {
    // The refractory is counted in analysis frames and the sweep needs raw mean/sigma, so a
    // decimated tier-1 window would model the wrong refractory against absent statistics.
    const h = makeHarness();
    const { result } = renderCal(h);
    act(() => result.current.start());
    feed(h, STEP_SECONDS.room, QUIET_ROOM);
    runStep(h, 0);
    feed(h, STEP_SECONDS.music, MUSIC);
    runStep(h, 0);

    expect(h.requestStream).toHaveBeenCalledWith(2, 32);
  });

  it("ends the tap step early once enough taps are in", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    act(() => result.current.start());
    feed(h, STEP_SECONDS.room, QUIET_ROOM);
    runStep(h, 0);
    feed(h, STEP_SECONDS.music, MUSIC);
    runStep(h, 0);
    expect(result.current.state.step).toBe("tap");

    // Feed matching audio while tapping in time with it.
    for (let i = 0; i < TAP_TARGET; i++) {
      feed(h, 0.512, MUSIC); // 16 frames = one planted beat
      act(() => result.current.recordTap());
    }
    expect(result.current.state.step).not.toBe("tap");
    // Back to the screen default once the burst is over.
    expect(h.requestStream).toHaveBeenLastCalledWith(null);
  });

  it("reaches review with changes, and still writes nothing", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    act(() => result.current.start());
    feed(h, STEP_SECONDS.room, QUIET_ROOM);
    runStep(h, 0);
    feed(h, STEP_SECONDS.music, MUSIC);
    runStep(h, 0);
    for (let i = 0; i < TAP_TARGET; i++) {
      feed(h, 0.512, MUSIC);
      act(() => result.current.recordTap());
    }

    expect(result.current.state.step).toBe("review");
    expect(result.current.state.changes.length).toBeGreaterThan(0);
    expect(h.writeParam).not.toHaveBeenCalled();
    expect(h.snapshotPreset).not.toHaveBeenCalled();
  });

  it("proposes only parameters that actually changed", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    act(() => result.current.start());
    feed(h, STEP_SECONDS.room, QUIET_ROOM);
    runStep(h, 0);
    feed(h, STEP_SECONDS.music, MUSIC);
    runStep(h, 0);
    for (let i = 0; i < TAP_TARGET; i++) {
      feed(h, 0.512, MUSIC);
      act(() => result.current.recordTap());
    }
    for (const ch of result.current.state.changes) {
      expect(ch.oldValue).not.toBe(ch.newValue);
      expect(ch.because.length).toBeGreaterThan(0);
      expect(ch.label).toBe(
        AUDIO_PARAMS[ch.key as AudioParamKey].friendlyLabel,
      );
    }
  });

  it("leaves the sensitivity alone when there are too few taps, and says so", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    act(() => result.current.start());
    feed(h, STEP_SECONDS.room, QUIET_ROOM);
    runStep(h, 0);
    feed(h, STEP_SECONDS.music, MUSIC);
    runStep(h, 0);

    // Three taps, then let the step time out.
    for (let i = 0; i < 3; i++) {
      feed(h, 0.512, MUSIC);
      act(() => result.current.recordTap());
    }
    runStep(h, STEP_SECONDS.tap);

    expect(result.current.state.step).toBe("review");
    const keys = result.current.state.changes.map((c) => c.key);
    expect(keys).not.toContain("beatAlpha");
    expect(keys).not.toContain("beatSfDelta");
    expect(result.current.state.notes.join(" ")).toContain(
      "left the sensitivity alone",
    );
  });

  it("refuses to fit a sensitivity to a decimated window", () => {
    // 8 Hz frames: the refractory is counted in frames, so fitting one here would model a
    // four-times-longer refractory than the device applies.
    const h = makeHarness();
    const { result } = renderCal(h);
    act(() => result.current.start());
    feed(h, STEP_SECONDS.room, QUIET_ROOM);
    runStep(h, 0);
    feed(h, STEP_SECONDS.music, MUSIC);
    runStep(h, 0);
    for (let i = 0; i < TAP_TARGET; i++) {
      feed(h, 0.5, MUSIC, 8);
      act(() => result.current.recordTap());
    }
    expect(result.current.state.notes.join(" ")).toContain("fast enough");
    expect(result.current.state.changes.map((c) => c.key)).not.toContain(
      "beatAlpha",
    );
  });

  describe("apply", () => {
    function reachReview(h: Harness) {
      const rendered = renderCal(h);
      act(() => rendered.result.current.start());
      feed(h, STEP_SECONDS.room, QUIET_ROOM);
      runStep(h, 0);
      feed(h, STEP_SECONDS.music, MUSIC);
      runStep(h, 0);
      for (let i = 0; i < TAP_TARGET; i++) {
        feed(h, 0.512, MUSIC);
        act(() => rendered.result.current.recordTap());
      }
      return rendered;
    }

    it("snapshots the old settings BEFORE the first write", async () => {
      const h = makeHarness();
      const { result } = reachReview(h);
      const changes = result.current.state.changes;

      await act(async () => {
        await result.current.apply(changes);
      });

      expect(h.snapshotPreset).toHaveBeenCalledWith("Before calibration");
      expect(h.snapshotPreset.mock.invocationCallOrder[0]).toBeLessThan(
        h.writeParam.mock.invocationCallOrder[0],
      );
      expect(result.current.state.step).toBe("done");
    });

    it("writes every accepted change, and only those", async () => {
      const h = makeHarness();
      const { result } = reachReview(h);
      const changes = result.current.state.changes;
      const kept = changes.slice(0, 2);

      await act(async () => {
        await result.current.apply(kept);
      });

      expect(h.writeParam).toHaveBeenCalledTimes(kept.length);
      const written = h.writeParam.mock.calls.map((c) => c[0]);
      expect(written).toEqual(kept.map((c) => c.key));
    });

    it("stops at the first failed write rather than pressing on", async () => {
      // A partially applied fit is worse than none: the parameters interact, so half of them
      // is a combination nothing measured.
      const h = makeHarness();
      h.writeParam.mockResolvedValueOnce(true).mockResolvedValueOnce(false);
      const { result } = reachReview(h);
      const changes = result.current.state.changes;

      await act(async () => {
        await result.current.apply(changes);
      });

      expect(h.writeParam).toHaveBeenCalledTimes(2);
      expect(result.current.state.step).toBe("review");
      expect(result.current.state.applyError).toContain("Before calibration");
    });

    it("treats a thrown write as a failure, not a crash", async () => {
      const h = makeHarness();
      h.writeParam.mockRejectedValueOnce(new Error("disconnected"));
      const { result } = reachReview(h);

      await act(async () => {
        await result.current.apply(result.current.state.changes);
      });
      expect(result.current.state.step).toBe("review");
      expect(result.current.state.applyError).toBeTruthy();
    });

    it("applying nothing is a no-op that still finishes cleanly", async () => {
      const h = makeHarness();
      const { result } = reachReview(h);
      await act(async () => {
        await result.current.apply([]);
      });
      expect(h.writeParam).not.toHaveBeenCalled();
      expect(h.snapshotPreset).not.toHaveBeenCalled();
      expect(result.current.state.step).toBe("done");
    });
  });

  it("cancel returns to the intro and restores the default stream", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    act(() => result.current.start());
    act(() => result.current.cancel());
    expect(result.current.state.step).toBe("intro");
    expect(h.requestStream).toHaveBeenLastCalledWith(null);
  });
});
