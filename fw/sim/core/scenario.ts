/**
 * Scenario schema (rgbx-scenario/1) — deterministic input stimuli for a
 * simulation run. Files live in fw/sim/scenarios/*.json; the CLI also
 * accepts arbitrary paths. Types only here (platform-agnostic); provider
 * construction from a scenario lives in node/scenarioRun.ts (WAV/file
 * access is platform-specific).
 */

export interface ScenarioAudioSilence {
  type: "silence";
}
export interface ScenarioAudioMetronome {
  type: "metronome";
  bpm: number;
  clickHz?: number;
  clickMs?: number;
  gainDb?: number;
}
export interface ScenarioAudioSweep {
  type: "sweep";
  fromHz: number;
  toHz: number;
  durationMs?: number;
  gainDb?: number;
}
export interface ScenarioAudioNoise {
  type: "noise";
  color?: "white" | "pink";
  gainDb?: number;
  /** Defaults to the scenario seed for determinism. */
  seed?: number;
}
export interface ScenarioAudioWav {
  type: "wav";
  /** Path, resolved relative to the scenario file. */
  file: string;
}
/** Replays feature frames verbatim (bypasses the DSP): a D-line dump from
 * a device `sound dump` or the native_sim replay harness. */
export interface ScenarioAudioFeatures {
  type: "features";
  file: string;
}
export type ScenarioAudio =
  | ScenarioAudioSilence
  | ScenarioAudioMetronome
  | ScenarioAudioSweep
  | ScenarioAudioNoise
  | ScenarioAudioWav
  | ScenarioAudioFeatures;

export interface ScenarioImuStatic {
  type: "static";
  accel?: [number, number, number];
  gyro?: [number, number, number];
}
export interface ScenarioImuRamp {
  type: "ramp";
  fromAccel: [number, number, number];
  toAccel: [number, number, number];
  fromGyro?: [number, number, number];
  toGyro?: [number, number, number];
  startMs?: number;
  endMs?: number;
}
export interface ScenarioImuSine {
  type: "sine";
  channel?: "accel" | "gyro";
  axis: 0 | 1 | 2;
  amplitude: number;
  hz: number;
}
export interface ScenarioImuKeyframes {
  type: "keyframes";
  frames: { atMs: number; accel: [number, number, number]; gyro?: [number, number, number] }[];
}
export type ScenarioImu = ScenarioImuStatic | ScenarioImuRamp | ScenarioImuSine | ScenarioImuKeyframes;

/** Timeline events applied when simTime reaches atMs (each fires once).
 * `set` writes params by NAME (index-independent); COLOR values may be
 * given as "0xMMRRGGBB" strings carrying the mode byte; STRING params take
 * strings. `press` queues a one-tick button edge. */
export interface ScenarioEvent {
  atMs: number;
  set?: Record<string, number | string>;
  press?: "Up" | "Left" | "Right" | "Down" | "Wake";
}

export interface ScenarioExpect {
  /** At least one non-black raw frame by this sim time. */
  nonBlackBeforeMs?: number;
  /** The ×0.02-and-truncate frame must still contain a lit pixel — catches
   * the "renders at 32/255, invisible on the panel" trap. */
  visibleAfterBrightness?: boolean;
  /** Lit-pixel response on beat ticks must exceed off-beat ticks. */
  beatResponse?: boolean;
  /** The run is EXPECTED to fault this way (hello's Crash/Hang params);
   * completing without this fault fails the run. */
  fault?: { kind: "trap" | "wall_backstop" | "cpu_budget" };
}

export interface Scenario {
  schema: "rgbx-scenario/1";
  name: string;
  description: string;
  durationMs: number;
  seed?: number;
  audio?: ScenarioAudio;
  imu?: ScenarioImu;
  timeline?: ScenarioEvent[];
  expect?: ScenarioExpect;
}

export function parseScenario(json: unknown): Scenario {
  const s = json as Scenario;
  if (s === null || typeof s !== "object" || s.schema !== "rgbx-scenario/1") {
    throw new Error('scenario must have "schema": "rgbx-scenario/1"');
  }
  if (typeof s.name !== "string" || typeof s.durationMs !== "number") {
    throw new Error("scenario needs name and durationMs");
  }
  return s;
}

export const BUTTON_INDEX: Record<string, number> = {
  Up: 0,
  Left: 1,
  Right: 2,
  Down: 3,
  Wake: 4,
};
