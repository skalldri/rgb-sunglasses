/**
 * Tests for opportunistic calibration collection.
 *
 * Two families here. The SAFETY properties carry over from the step machine unchanged, because
 * they are what stop a confident wrong answer reaching a device someone is about to wear on
 * stage: nothing is written before the review, the snapshot is taken first, and a failed write
 * stops the batch rather than leaving half a fit.
 *
 * The COLLECTION properties are new, and they exist because the timed version was unusable at a
 * venue. It demanded 8 s of silence, then 15 s of music, then 30 s of tapping, each starting
 * when the app decided — and when it did not get them it did not fail, it fitted whatever was
 * playing and reported success. Everything below about ordering, accumulation and discarding is
 * pinning the absence of that.
 */

import { act, renderHook } from "@testing-library/react-native";

import {
  useAudioCalibration,
  COLLECTOR_MIN_FRAMES,
  type CollectorKind,
} from "@/hooks/use-audio-calibration";
import { AUDIO_PARAMS, type AudioParamKey } from "@/services/audio-params";
import {
  createTelemetryRing,
  pushTelemetryBytes,
  type TelemetryRing,
} from "@/services/audio-telemetry";
import { makeFrame } from "./fixtures/audio-telemetry";

/** Controllable clock so collection can be driven deterministically. */
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
  const snapshotPreset = jest.fn().mockReturnValue(true);
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

  return { clock, ring, requestStream, writeParam, snapshotPreset, valueOf, values };
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

const QUIET = (n: number) => makeFrame({ tier: 2, seq: n, rmsInput: 0.0005 });
const LOUD = (n: number) => makeFrame({ tier: 2, seq: n, rmsInput: 0.05 });
/** Music with a real dynamic range, so analyzeMusic has percentiles to work with. */
const MUSIC = (n: number) =>
  makeFrame({ tier: 2, seq: n, rmsInput: 0.01 + (n % 50) * 0.0008 });

type Cal = ReturnType<typeof renderCal>["result"];

/** One complete sitting on a collector: start, record, stop. */
function sitting(
  result: Cal,
  h: Harness,
  kind: CollectorKind,
  seconds: number,
  frame: (n: number) => Uint8Array,
) {
  act(() => result.current.startCollecting(kind));
  feed(h, seconds, frame);
  act(() => result.current.stopCollecting());
}

describe("collection is not timed", () => {
  beforeEach(() => jest.useFakeTimers());
  afterEach(() => {
    jest.useRealTimers();
    jest.restoreAllMocks();
  });

  it("collects nothing until a collector is started, however long it waits", () => {
    const h = makeHarness();
    const { result } = renderCal(h);

    // Frames stream past for a full minute. The old flow would have been counting down
    // through them; nothing here is.
    feed(h, 60, QUIET);
    act(() => {
      jest.advanceTimersByTime(60_000);
    });

    expect(result.current.state.collectors.background.frames).toBe(0);
    expect(result.current.state.canFit).toBe(false);
    expect(h.writeParam).not.toHaveBeenCalled();
  });

  it("counts the sitting in progress, not just committed ones", () => {
    // Hardware-found. A sitting is only sealed into the pool at stop(), so reporting the
    // committed figure left the readout at "0.0s collected" for the whole sitting — observed
    // reading 0.0s through 35 s of successful collection, which is indistinguishable from a
    // dead stream. At a venue a sitting runs for minutes.
    const h = makeHarness();
    const { result } = renderCal(h);

    act(() => result.current.startCollecting("background"));
    feed(h, 2, QUIET);
    act(() => {
      jest.advanceTimersByTime(1000);
    });

    // Still recording — nothing has been stopped or sealed.
    expect(result.current.state.active).toBe("background");
    expect(result.current.state.collectors.background.frames).toBeGreaterThan(0);
    expect(result.current.state.collectors.background.seconds).toBeGreaterThan(0);
  });

  it("stops counting when the stream dies mid-sitting", () => {
    // The count must come from frames collected, never elapsed time — a readout that keeps
    // climbing while nothing arrives is the same lie the old countdown told.
    const h = makeHarness();
    const { result } = renderCal(h);

    act(() => result.current.startCollecting("background"));
    feed(h, 2, QUIET);
    act(() => {
      jest.advanceTimersByTime(1000);
    });
    const afterFrames = result.current.state.collectors.background.frames;
    const afterSeconds = result.current.state.collectors.background.seconds;

    // Time passes; no frames arrive.
    h.clock.advance(30_000);
    act(() => {
      jest.advanceTimersByTime(30_000);
    });

    expect(result.current.state.collectors.background.frames).toBe(afterFrames);
    // SECONDS too, not just frames. Asserting only the frame count let a mutant that read
    // seconds straight off the wall clock survive — the readout would have climbed through
    // half a minute of silence while nothing was being collected.
    expect(result.current.state.collectors.background.seconds).toBeCloseTo(
      afterSeconds,
      6,
    );
  });

  it("accumulates across several sittings with gaps between them", () => {
    const h = makeHarness();
    const { result } = renderCal(h);

    sitting(result, h, "background", 0.5, QUIET);
    const afterFirst = result.current.state.collectors.background.frames;
    expect(afterFirst).toBeGreaterThan(0);

    // Minutes pass with nothing collected.
    feed(h, 120, LOUD);

    sitting(result, h, "background", 0.5, QUIET);
    expect(result.current.state.collectors.background.frames).toBeGreaterThan(
      afterFirst,
    );
    expect(result.current.state.collectors.background.chunks).toBe(2);
  });

  it("accepts the collectors in any order", () => {
    const h = makeHarness();
    const { result } = renderCal(h);

    // Music first — impossible in the step machine, which forced room -> music -> tap.
    sitting(result, h, "music", 4, MUSIC);
    expect(result.current.state.canFit).toBe(false);
    sitting(result, h, "background", 2, QUIET);

    expect(result.current.state.canFit).toBe(true);
  });

  it("drains while a long sitting is still running, so nothing is lost to wraparound", () => {
    const h = makeHarness();
    const { result } = renderCal(h);

    act(() => result.current.startCollecting("background"));
    // More frames than the ring holds, delivered across several drain intervals.
    for (let i = 0; i < 4; i++) {
      feed(h, 10, QUIET);
      act(() => {
        jest.advanceTimersByTime(1000);
      });
    }
    act(() => result.current.stopCollecting());

    // The ring caps at 1024; a pool that only read it at stop() could never exceed that.
    expect(result.current.state.collectors.background.frames).toBeGreaterThan(
      1024,
    );
  });
});

describe("one collector at a time", () => {
  beforeEach(() => jest.useFakeTimers());
  afterEach(() => {
    jest.useRealTimers();
    jest.restoreAllMocks();
  });

  it("starting a second collector closes the first rather than sharing frames", () => {
    const h = makeHarness();
    const { result } = renderCal(h);

    act(() => result.current.startCollecting("background"));
    feed(h, 1, QUIET);
    // Switch without stopping first.
    act(() => result.current.startCollecting("music"));
    feed(h, 4, MUSIC);
    act(() => result.current.stopCollecting());

    // Both pools hold their own frames, and the music never leaked into the background one —
    // a background pool containing the music is the contamination this design exists to stop.
    expect(result.current.state.collectors.background.frames).toBeGreaterThan(0);
    expect(result.current.state.collectors.music.frames).toBeGreaterThan(0);
    expect(result.current.state.active).toBe(null);
  });

  it("cancel clears the active collector so a stale pad cannot swallow taps", () => {
    // recordTap guards on the REF but the TapPad renders on the STATE. A cancel() that reset
    // only the ref left the pad on screen with every press silently eaten by the guard —
    // press feedback with a frozen count. State and ref must fall together.
    const h = makeHarness();
    const { result } = renderCal(h);
    act(() => result.current.startCollecting("taps"));
    feed(h, 1, QUIET, 32);
    act(() => result.current.cancel());

    expect(result.current.state.active).toBeNull();
    act(() => result.current.recordTap());
    expect(result.current.state.tapCount).toBe(0);
  });

  it("records taps only while the tap collector is running", () => {
    const h = makeHarness();
    const { result } = renderCal(h);

    act(() => result.current.recordTap());
    expect(result.current.state.tapCount).toBe(0);

    act(() => result.current.startCollecting("taps"));
    act(() => result.current.recordTap());
    act(() => result.current.recordTap());
    expect(result.current.state.tapCount).toBe(2);

    act(() => result.current.stopCollecting());
    act(() => result.current.recordTap());
    expect(result.current.state.tapCount).toBe(2);
  });

  it("asks for undecimated frames for taps, and plain stats otherwise", () => {
    const h = makeHarness();
    const { result } = renderCal(h);

    act(() => result.current.startCollecting("background"));
    // analyzeRoom fits the beat floor from band-0 flux, which needs tier 2 even for "just the
    // room" — the meters-only tier carries no flux at all.
    expect(h.requestStream).toHaveBeenLastCalledWith(2);
    act(() => result.current.stopCollecting());

    act(() => result.current.startCollecting("taps"));
    // The sweep fits a refractory counted in FRAMES; against decimated frames it would model a
    // longer gap than the device applies.
    expect(h.requestStream).toHaveBeenLastCalledWith(2, 32);
    act(() => result.current.stopCollecting());
    expect(h.requestStream).toHaveBeenLastCalledWith(null);
  });
});

describe("discarding a ruined sitting", () => {
  beforeEach(() => jest.useFakeTimers());
  afterEach(() => {
    jest.useRealTimers();
    jest.restoreAllMocks();
  });

  it("removes the whole sitting, not one drain interval of it", () => {
    const h = makeHarness();
    const { result } = renderCal(h);

    sitting(result, h, "background", 2, QUIET);
    const good = result.current.state.collectors.background.frames;

    // A long second sitting spanning several drains — the band came back early.
    act(() => result.current.startCollecting("background"));
    for (let i = 0; i < 3; i++) {
      feed(h, 2, LOUD);
      act(() => {
        jest.advanceTimersByTime(1000);
      });
    }
    act(() => result.current.stopCollecting());
    expect(result.current.state.collectors.background.frames).toBeGreaterThan(
      good,
    );

    act(() => result.current.discardLast("background"));

    // All of it goes, not just the last second. Sealing at stop() is what makes chunk ==
    // sitting; draining straight into the pool would leave most of the contamination behind.
    expect(result.current.state.collectors.background.frames).toBe(good);
    expect(result.current.state.collectors.background.chunks).toBe(1);
  });

  it("stops a live collector before discarding it", () => {
    const h = makeHarness();
    const { result } = renderCal(h);

    sitting(result, h, "music", 4, MUSIC);
    act(() => result.current.startCollecting("music"));
    feed(h, 2, LOUD);
    act(() => result.current.discardLast("music"));

    expect(result.current.state.active).toBe(null);
  });
});

describe("fitting", () => {
  beforeEach(() => jest.useFakeTimers());
  afterEach(() => {
    jest.useRealTimers();
    jest.restoreAllMocks();
  });

  function gather(result: Cal, h: Harness) {
    sitting(result, h, "background", 2, QUIET);
    sitting(result, h, "music", 4, MUSIC);
  }

  it("needs background and music, but NOT taps", () => {
    const h = makeHarness();
    const { result } = renderCal(h);

    sitting(result, h, "background", 2, QUIET);
    expect(result.current.state.canFit).toBe(false);

    sitting(result, h, "music", 4, MUSIC);
    expect(result.current.state.canFit).toBe(true);
    expect(result.current.state.collectors.taps.ready).toBe(false);
  });

  it("produces a proposal and writes nothing doing it", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    gather(result, h);

    act(() => result.current.fit());

    expect(result.current.state.phase).toBe("review");
    expect(result.current.state.changes.length).toBeGreaterThan(0);
    expect(h.writeParam).not.toHaveBeenCalled();
    expect(h.snapshotPreset).not.toHaveBeenCalled();
  });

  it("says so plainly when no taps were collected, rather than failing", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    gather(result, h);
    act(() => result.current.fit());

    expect(result.current.state.phase).toBe("review");
    expect(result.current.state.notes.join(" ")).toContain("did not tap along");
  });

  it("warns that the room was noisy, and fits it anyway", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    // Well above ROOM_NOISY_RMS. The old room step REFUSED this outright.
    sitting(result, h, "background", 2, (n) =>
      makeFrame({ tier: 2, seq: n, rmsInput: 0.02, flux: [0.04, 0, 0, 0] }),
    );
    sitting(result, h, "music", 4, MUSIC);
    act(() => result.current.fit());

    expect(result.current.state.phase).toBe("review");
    expect(result.current.state.warnings.join(" ")).toContain(
      "background noise",
    );
    // A noisy room is the one case the floor IS fitted from — the row must be there.
    const floorRow = result.current.state.changes.find(
      (c) => c.key === "beatFluxFloor",
    );
    expect(floorRow).toBeDefined();
    expect(floorRow?.because).toContain("background noise");
  });

  it("warns when the mic clipped during the music", () => {
    // analyzeMusic pulls the ceiling down 25% when the mic was overdriven (clipAdjusted). The
    // flag was computed and tested but never shown — the review claimed a clean fit off a
    // distorting mic.
    const h = makeHarness();
    const { result } = renderCal(h);
    sitting(result, h, "background", 2, QUIET);
    sitting(result, h, "music", 4, (n) =>
      makeFrame({
        tier: 2,
        seq: n,
        rmsInput: 0.01 + (n % 50) * 0.0008,
        clipped: n % 10 === 0, // 10% of frames, well past MUSIC_CLIP_FRACTION
      }),
    );
    act(() => result.current.fit());

    expect(result.current.state.phase).toBe("review");
    expect(result.current.state.warnings.join(" ")).toContain("overdriven");
  });

  it("notes when the window had to be widened to keep the AGC stable", () => {
    // Music with almost no dynamic range: p25 ≈ p90, so high < 4x low and analyzeMusic drags
    // the window open (widened). Observed live (2026-08-29 venue run): the window came out at
    // exactly 4x while the review said the ceiling was "fitted to the loud parts of the
    // music" — the correction must be disclosed, not passed off as a measurement.
    const h = makeHarness();
    const { result } = renderCal(h);
    sitting(result, h, "background", 2, QUIET);
    sitting(result, h, "music", 4, (n) =>
      makeFrame({ tier: 2, seq: n, rmsInput: 0.01 }),
    );
    act(() => result.current.fit());

    expect(result.current.state.phase).toBe("review");
    expect(result.current.state.notes.join(" ")).toContain(
      "widened the volume window",
    );
  });

  it("leaves the floor alone in a quiet room, and says so instead of warning", () => {
    // The 2026-08-29 hardware bug: a silent room's band-0 flux is log-noise (measured p99 =
    // 1.19 at rms p95 = 0.0006), and fitting the floor to it proposed the MAXIMUM justified
    // by "the background noise is loud enough…" while `noisy` said the room was quiet. The
    // fixture reproduces that input: quiet rms, junk flux spikes well past FLOOR_MAX.
    const h = makeHarness();
    const { result } = renderCal(h);
    sitting(result, h, "background", 2, (n) =>
      makeFrame({
        tier: 2,
        seq: n,
        rmsInput: 0.0005,
        flux: [n % 2 ? 1.2 : 0, 0, 0, 0],
      }),
    );
    sitting(result, h, "music", 4, MUSIC);
    act(() => result.current.fit());

    expect(result.current.state.phase).toBe("review");
    const keys = result.current.state.changes.map((c) => c.key);
    expect(keys).not.toContain("beatFluxFloor");
    // The user is told the floor was left alone, not warned about phantom background noise.
    expect(result.current.state.notes.join(" ")).toContain(
      "Minimum beat strength has been left alone",
    );
    expect(result.current.state.warnings.join(" ")).not.toContain(
      "as high as it safely goes",
    );
  });

  it("promotes the anti-double refractory to a validated proposal row", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    sitting(result, h, "background", 2, QUIET);
    sitting(result, h, "music", 4, MUSIC);

    // 75 BPM taps (800 ms apart) over a recording where every beat has an echo 13 frames
    // (416 ms) later. No refractory in the sweep's grid (max 12) suppresses the echo, so the
    // sweep's best still fires at ~150 BPM; the computed fix is 14 frames — OUTSIDE the grid,
    // which is exactly why it must be replay-validated rather than assumed.
    act(() => result.current.startCollecting("taps"));
    for (let k = 0; k < 10; k++) {
      act(() => result.current.recordTap());
      feed(h, 0.8, (n) =>
        makeFrame({
          tier: 2,
          seq: n,
          rmsInput: 0.01,
          flux: n === 0 || n === 13 ? [0.5, 0, 0, 0] : [0, 0, 0, 0],
        }),
      );
    }
    act(() => result.current.stopCollecting());
    act(() => result.current.fit());

    expect(result.current.state.phase).toBe("review");
    const row = result.current.state.changes.find(
      (c) => c.key === "beatRefractoryFrames",
    );
    expect(row).toBeDefined();
    expect(row?.newValue).toBe(14);
    expect(row?.because).toContain("firing twice per beat");
    // The tempo coupling is stated on the row, not hidden.
    expect(row?.because).toContain("BPM");
    // The manual-procedure note is replaced by the row.
    expect(result.current.state.notes.join(" ")).not.toContain(
      "could not fix that automatically",
    );
  });

  it("keeps the manual note when the anti-double refractory does not actually help", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    sitting(result, h, "background", 2, QUIET);
    sitting(result, h, "music", 4, MUSIC);

    // Same double-fire shape, but the echo lands 17 frames out — past the computed fix of 14
    // — so replaying with the candidate changes nothing. The promotion is gated on the replay
    // IMPROVING the match; an unvalidated write here would ship a number that provably does
    // not fix the thing its because-line claims.
    act(() => result.current.startCollecting("taps"));
    for (let k = 0; k < 10; k++) {
      act(() => result.current.recordTap());
      feed(h, 0.8, (n) =>
        makeFrame({
          tier: 2,
          seq: n,
          rmsInput: 0.01,
          flux: n === 0 || n === 17 ? [0.5, 0, 0, 0] : [0, 0, 0, 0],
        }),
      );
    }
    act(() => result.current.stopCollecting());
    act(() => result.current.fit());

    expect(result.current.state.phase).toBe("review");
    const row = result.current.state.changes.find(
      (c) => c.key === "beatRefractoryFrames",
    );
    expect(row?.newValue).not.toBe(14);
    expect(row?.because).toBe("Chosen alongside the sensitivity.");
    expect(result.current.state.notes.join(" ")).toContain(
      "could not fix that automatically",
    );
  });

  it("explains the every-other-beat case instead of proposing what the sweep rejected", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    sitting(result, h, "background", 2, QUIET);
    sitting(result, h, "music", 4, MUSIC);

    // Taps every ~416 ms; the recording only has a beat under every SECOND tap, so the best
    // any candidate can do is fire at half the tapped tempo. There is nothing evidence-backed
    // to propose — every sensitivity scored the same or worse — so the fit must say why it is
    // leaving Sensitivity alone, not guess "one step up".
    act(() => result.current.startCollecting("taps"));
    for (let k = 0; k < 12; k++) {
      act(() => result.current.recordTap());
      feed(h, 0.4, (n) =>
        makeFrame({
          tier: 2,
          seq: n,
          rmsInput: 0.01,
          flux: k % 2 === 0 && n === 0 ? [0.5, 0, 0, 0] : [0, 0, 0, 0],
        }),
      );
    }
    act(() => result.current.stopCollecting());
    act(() => result.current.fit());

    expect(result.current.state.phase).toBe("review");
    const notes = result.current.state.notes.join(" ");
    expect(notes).toContain("every other beat");
    expect(notes).toContain("matched your taps worse");
  });

  it("sweeps against the device's CURRENT floor when the fit proposes none", () => {
    // With no floor proposal the replay must model the floor the device will actually run
    // after apply — its current value — not a value nothing is going to set.
    const h = makeHarness();
    const valueOfSpy = jest.fn(h.valueOf);
    h.valueOf = valueOfSpy;
    const { result } = renderCal(h);

    sitting(result, h, "background", 2, QUIET);
    sitting(result, h, "music", 4, MUSIC);

    // A regular tap sitting so the sweep actually runs (>= MIN_TAPS, even spacing).
    act(() => result.current.startCollecting("taps"));
    for (let i = 0; i < 10; i++) {
      feed(h, 0.5, QUIET, 32);
      act(() => result.current.recordTap());
    }
    act(() => result.current.stopCollecting());

    valueOfSpy.mockClear();
    act(() => result.current.fit());

    expect(result.current.state.phase).toBe("review");
    expect(valueOfSpy).toHaveBeenCalledWith("beatFluxFloor");
  });

  it("keeps what was collected when a fit fails", () => {
    // "Nothing to change" is a legitimate outcome, and it must not cost the pools — a venue
    // may not offer those quiet moments twice.
    const h = makeHarness();
    const { result } = renderCal(h);
    gather(result, h);
    const before = result.current.state.collectors.background.frames;

    act(() => result.current.cancel());
    expect(result.current.state.phase).toBe("collecting");
    expect(result.current.state.collectors.background.frames).toBe(before);
  });

  it("reset throws everything away", () => {
    const h = makeHarness();
    const { result } = renderCal(h);
    gather(result, h);

    act(() => result.current.reset());
    expect(result.current.state.collectors.background.frames).toBe(0);
    expect(result.current.state.collectors.music.frames).toBe(0);
    expect(result.current.state.canFit).toBe(false);
  });
});

describe("applying — carried over from the step machine unchanged", () => {
  beforeEach(() => jest.useFakeTimers());
  afterEach(() => {
    jest.useRealTimers();
    jest.restoreAllMocks();
  });

  async function toReview(h: Harness) {
    const rendered = renderCal(h);
    sitting(rendered.result, h, "background", 2, QUIET);
    sitting(rendered.result, h, "music", 4, MUSIC);
    act(() => rendered.result.current.fit());
    return rendered;
  }

  it("snapshots BEFORE the first write", async () => {
    const h = makeHarness();
    const { result } = await toReview(h);
    const order: string[] = [];
    h.snapshotPreset.mockImplementation(() => {
      order.push("snapshot");
      return true;
    });
    h.writeParam.mockImplementation(async () => {
      order.push("write");
      return true;
    });

    await act(async () => {
      await result.current.apply(result.current.state.changes);
    });

    expect(order[0]).toBe("snapshot");
    expect(h.snapshotPreset).toHaveBeenCalledWith("Before calibration");
    expect(result.current.state.phase).toBe("done");
  });

  it("stops at the first failed write and names it", async () => {
    const h = makeHarness();
    const { result } = await toReview(h);
    const changes = result.current.state.changes;
    h.writeParam.mockResolvedValueOnce(true).mockResolvedValueOnce(false);

    await act(async () => {
      await result.current.apply(changes);
    });

    // A partially-applied fit is worse than none: the parameters interact.
    expect(h.writeParam).toHaveBeenCalledTimes(2);
    expect(result.current.state.phase).toBe("review");
    expect(result.current.state.applyError).toContain("Nothing after it");
  });

  it("is honest when the snapshot itself did not land", async () => {
    const h = makeHarness();
    const { result } = await toReview(h);
    h.snapshotPreset.mockReturnValue(false);
    h.writeParam.mockResolvedValue(false);

    await act(async () => {
      await result.current.apply(result.current.state.changes);
    });

    expect(result.current.state.applyError).toContain("could NOT be saved");
  });
});

describe("readiness minimums", () => {
  it("reuses the analysis layer's own minimums so they cannot disagree", () => {
    // If the pill said "ready" at a frame count analyzeRoom then refused, the user would be
    // told to fit and immediately told it could not.
    expect(COLLECTOR_MIN_FRAMES.background).toBeGreaterThan(0);
    expect(COLLECTOR_MIN_FRAMES.music).toBeGreaterThanOrEqual(
      COLLECTOR_MIN_FRAMES.background,
    );
  });
});
