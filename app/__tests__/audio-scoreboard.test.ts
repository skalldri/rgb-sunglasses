import {
  CLIP_FRACTION,
  MUTED_FRACTION,
  TOO_SENSITIVE_BPS,
  computeVerdict,
} from "@/services/audio-scoreboard";
import { AUDIO_PARAMS } from "@/services/audio-params";
import {
  EMPTY_SUMMARY,
  type TelemetrySummary,
} from "@/services/audio-telemetry";

/** A healthy stream: live, in the AGC window, beating steadily. */
function healthy(over: Partial<TelemetrySummary> = {}): TelemetrySummary {
  return {
    ...EMPTY_SUMMARY,
    frames: 80,
    live: true,
    ageMs: 120,
    tier: 1,
    gainSteps: 13,
    gainDb: 6.5,
    rmsInputDb: -34,
    noiseFloorDb: -64,
    peakDb: -13,
    headroomDb: 13,
    beatsPerSecond: 2.1,
    bpm: 128,
    lastBeatBand: 0,
    ...over,
  };
}

describe("computeVerdict priority", () => {
  it("distinguishes a stream that never started from one that died", () => {
    // The discriminator is ageMs, not the frame count: when a stream dies the window drains
    // to zero frames, which used to read as a fresh start at exactly the wrong moment.
    const neverStarted = computeVerdict({
      ...healthy(),
      frames: 0,
      ageMs: Infinity,
    });
    expect(neverStarted.kind).toBe("no-data");

    const diedAndDrained = computeVerdict({
      ...healthy(),
      frames: 0,
      live: false,
      ageMs: 30_000,
    });
    expect(diedAndDrained.kind).toBe("stale");
    expect(diedAndDrained.title).toBe("No signal");
  });

  it("reports no data before anything else", () => {
    // Every other field is screaming, but with no frames we know nothing and must say so.
    const v = computeVerdict({
      ...healthy({ silentFraction: 1, clipFraction: 1, beatsPerSecond: 99 }),
      frames: 0,
    });
    expect(v.kind).toBe("no-data");
    expect(v.tone).toBe("neutral");
  });

  it("reports a stalled stream before diagnosing the audio", () => {
    // Stale numbers must never be presented as a live diagnosis.
    const v = computeVerdict(
      healthy({ live: false, silentFraction: 1, clipFraction: 1 }),
    );
    expect(v.kind).toBe("stale");
  });

  it("ranks muted above clipping, gain and both sensitivity verdicts", () => {
    // The ordering that matters most: turning sensitivity up while the gate eats the music
    // does nothing, so the gate has to be named first.
    const v = computeVerdict(
      healthy({
        silentFraction: 0.6,
        clipFraction: 1,
        gainPinnedHigh: true,
        beatsPerSecond: 0,
      }),
    );
    expect(v.kind).toBe("muted");
  });

  it("ranks clipping above a pinned mic", () => {
    const v = computeVerdict(
      healthy({ clipFraction: 0.5, gainPinnedHigh: true }),
    );
    expect(v.kind).toBe("clipping");
  });

  it("ranks a pinned mic above sensitivity advice", () => {
    // No amount of sensitivity fixes a room that is too quiet where the glasses are.
    const v = computeVerdict(
      healthy({ gainPinnedHigh: true, beatsPerSecond: 0 }),
    );
    expect(v.kind).toBe("gain-pinned");
  });

  it("falls through to good when nothing is wrong", () => {
    expect(computeVerdict(healthy()).kind).toBe("good");
    expect(computeVerdict(healthy()).tone).toBe("good");
  });
});

describe("computeVerdict thresholds", () => {
  it("calls muted strictly above the threshold, not at it", () => {
    expect(
      computeVerdict(healthy({ silentFraction: MUTED_FRACTION })).kind,
    ).toBe("good");
    expect(
      computeVerdict(healthy({ silentFraction: MUTED_FRACTION + 0.01 })).kind,
    ).toBe("muted");
  });

  it("calls clipping strictly above the threshold", () => {
    expect(computeVerdict(healthy({ clipFraction: CLIP_FRACTION })).kind).toBe(
      "good",
    );
    expect(
      computeVerdict(healthy({ clipFraction: CLIP_FRACTION + 0.01 })).kind,
    ).toBe("clipping");
  });

  it("calls too-sensitive strictly above the threshold", () => {
    expect(
      computeVerdict(healthy({ beatsPerSecond: TOO_SENSITIVE_BPS })).kind,
    ).toBe("good");
    expect(
      computeVerdict(healthy({ beatsPerSecond: TOO_SENSITIVE_BPS + 0.1 })).kind,
    ).toBe("too-sensitive");
  });

  it("reports insensitive only with enough evidence", () => {
    // Two frames of silence at the start of a stream is not a diagnosis.
    expect(computeVerdict(healthy({ frames: 3, beatsPerSecond: 0 })).kind).toBe(
      "good",
    );
    expect(
      computeVerdict(healthy({ frames: 80, beatsPerSecond: 0 })).kind,
    ).toBe("insensitive");
  });

  it("does not call a quiet room insensitive when it is really muted", () => {
    const v = computeVerdict(
      healthy({ beatsPerSecond: 0, silentFraction: 0.9 }),
    );
    expect(v.kind).toBe("muted");
  });
});

describe("verdict copy", () => {
  // This is product surface. Pinning it means a reworded sentence is a deliberate diff.
  it("pins the muted sentence, including the measured percentage", () => {
    const v = computeVerdict(healthy({ silentFraction: 0.47 }));
    expect(v.title).toBe("Muted 47% of the time");
    expect(v.detail).toBe(
      "The glasses think the room is quiet, so the lights stop reacting. Turn down Ignore background noise.",
    );
  });

  it("pins every other verdict sentence", () => {
    expect(computeVerdict({ ...healthy(), frames: 0 })).toMatchObject({
      title: "Not listening yet",
      detail: "Waiting for the glasses to send what they are hearing.",
    });
    expect(computeVerdict(healthy({ live: false }))).toMatchObject({
      title: "No signal",
      detail:
        "The glasses stopped sending. These numbers are the last ones received.",
    });
    expect(computeVerdict(healthy({ clipFraction: 0.5 }))).toMatchObject({
      title: "The mic is overloading",
      detail:
        "It backs off on its own, but you can lower Loudest we want to help it along.",
    });
    expect(computeVerdict(healthy({ gainPinnedHigh: true }))).toMatchObject({
      title: "Mic is at full gain",
      detail:
        "The music is quiet where the glasses are. Move closer to a speaker before changing anything here.",
    });
    expect(computeVerdict(healthy({ beatsPerSecond: 9 }))).toMatchObject({
      title: "Too sensitive",
      detail:
        "It is firing on almost every sound, including the snare. Turn Sensitivity down.",
    });
    expect(computeVerdict(healthy({ beatsPerSecond: 0 }))).toMatchObject({
      title: "Not finding beats",
      detail:
        "The glasses can hear the room but nothing is firing. Turn Sensitivity up.",
    });
  });

  it("names a control the Simple tab actually shows, for every actionable verdict", () => {
    // A verdict that names no control leaves the user with a diagnosis and no fix.
    //
    // The control names are read from AUDIO_PARAMS rather than hard-coded here, so this is a
    // real drift guard: renaming a parameter's friendlyLabel without updating the verdict
    // copy fails this test instead of silently telling someone to adjust a control that no
    // longer exists by that name. 'Move closer' is the one exception — it is a physical
    // instruction, not a control.
    const controls = [
      ...Object.values(AUDIO_PARAMS).map((p) => p.friendlyLabel),
      "Move closer",
    ];
    // Guard the guard: if the table stopped carrying these, the check above would pass
    // vacuously against a list of labels that no longer name anything actionable.
    expect(controls).toEqual(
      expect.arrayContaining([
        "Ignore background noise",
        "Loudest we want",
        "Sensitivity",
      ]),
    );
    const actionable = [
      healthy({ silentFraction: 0.9 }),
      healthy({ clipFraction: 0.5 }),
      healthy({ gainPinnedHigh: true }),
      healthy({ beatsPerSecond: 9 }),
      healthy({ beatsPerSecond: 0 }),
    ];
    for (const s of actionable) {
      const v = computeVerdict(s);
      expect(controls.some((c) => v.detail.includes(c))).toBe(true);
    }
  });

  it("mentions BPM in the good verdict only when it has one", () => {
    expect(computeVerdict(healthy({ bpm: 128 })).detail).toBe(
      "Steady beats at about 128 BPM.",
    );
    expect(computeVerdict(healthy({ bpm: null })).detail).toBe("Steady beats.");
  });

  it("gives every verdict a tone and a non-empty sentence", () => {
    const all = [
      { ...healthy(), frames: 0 },
      healthy({ live: false }),
      healthy({ silentFraction: 0.9 }),
      healthy({ clipFraction: 0.5 }),
      healthy({ gainPinnedHigh: true }),
      healthy({ beatsPerSecond: 9 }),
      healthy({ beatsPerSecond: 0 }),
      healthy(),
    ];
    const kinds = new Set<string>();
    for (const s of all) {
      const v = computeVerdict(s);
      kinds.add(v.kind);
      expect(v.title.length).toBeGreaterThan(0);
      expect(v.detail.length).toBeGreaterThan(0);
      expect(["neutral", "good", "warning", "bad"]).toContain(v.tone);
    }
    // Every branch reachable — a verdict nobody can trigger is dead copy.
    expect(kinds.size).toBe(8);
  });
});
