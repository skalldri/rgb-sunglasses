/**
 * Scenario playback for the browser UI: loads rgbx-scenario/1 files (bundled
 * via /scenario-index.json, or picked from disk) and builds the same
 * providers the Node CLI uses (core/scenarioProviders + core/scenarioTimeline),
 * so a replay in the browser computes exactly what `rgbx-sim run` computes.
 *
 * While a scenario is active its providers OVERRIDE the manual audio/IMU
 * sources — main.ts's tap providers fall back to the manual sources whenever
 * audioProvider/imuProvider here are null. Playback is frame-index driven
 * (the sim clock, not wall time), so pause/Step work unchanged and a restart
 * reproduces the identical run.
 */

import { AudioFeatureProvider, ImuProvider } from "../core/providers";
import { Scenario, parseScenario } from "../core/scenario";
import { ScenarioIo, buildAudioProvider, buildImuProvider } from "../core/scenarioProviders";
import { TimelineRunner } from "../core/scenarioTimeline";
import type { SimHost } from "../core/host";

/** One /scenario-index.json row (mirrors vite.config.ts's ScenarioEntry). */
export interface ScenarioIndexEntry {
  name: string;
  /** Relative to the deploy base, like the wasm index urls. */
  url: string;
  description: string;
  durationMs: number;
}

export interface LoadedScenario {
  scenario: Scenario;
  io: ScenarioIo;
}

const BASE = import.meta.env.BASE_URL;

/** audio_dsp.wasm bytes, fetched once and shared by every scenario run (the
 * provider instantiates its own wasm instance from these bytes, so runs stay
 * independent — only the fetch is cached). */
let dspBytesPromise: Promise<ArrayBuffer> | null = null;
function getDspBytes(): Promise<ArrayBuffer> {
  dspBytesPromise ??= (async () => {
    const resp = await fetch(`${BASE}audio_dsp.wasm`);
    if (!resp.ok) {
      throw new Error("audio_dsp.wasm not available (run fw/sim/build-extensions.sh)");
    }
    return resp.arrayBuffer();
  })().catch((err: unknown) => {
    // ANY failure may be transient (network blip, artifact not built yet) —
    // drop the cached rejection so the next Play retries the fetch.
    dspBytesPromise = null;
    throw err;
  });
  return dspBytesPromise;
}

export async function loadScenarioIndex(): Promise<ScenarioIndexEntry[]> {
  try {
    const resp = await fetch(`${BASE}scenario-index.json`);
    return (await resp.json()) as ScenarioIndexEntry[];
  } catch {
    return [];
  }
}

/** Fetches one bundled scenario; its `file:` refs resolve relative to the
 * scenario's own URL (so "assets/x.wav" works at "/" and "/sim/" alike). */
export async function fetchScenario(url: string): Promise<LoadedScenario> {
  const scenarioUrl = new URL(`${BASE}${url}`, window.location.href);
  const resp = await fetch(scenarioUrl);
  if (!resp.ok) {
    throw new Error(`HTTP ${resp.status} fetching ${url}`);
  }
  const scenario = parseScenario(await resp.json());
  const fetchRef = async (ref: string): Promise<Response> => {
    const r = await fetch(new URL(ref, scenarioUrl));
    if (!r.ok) {
      throw new Error(`HTTP ${r.status} fetching scenario file "${ref}"`);
    }
    return r;
  };
  return {
    scenario,
    io: {
      readBytes: async (ref) => new Uint8Array(await (await fetchRef(ref)).arrayBuffer()),
      readText: async (ref) => (await fetchRef(ref)).text(),
      getDspBytes,
    },
  };
}

/** Builds a scenario from files picked together on disk (the JSON plus its
 * WAV/feature assets). A lone JSON cannot carry its `file:` refs, so refs
 * resolve by BASENAME against the picked set — matching the layout
 * capture_to_scenario.py writes (<name>.json + assets/<name>.wav). */
export async function loadLocalScenario(files: File[]): Promise<LoadedScenario> {
  const jsons = files.filter((f) => f.name.toLowerCase().endsWith(".json"));
  if (jsons.length !== 1) {
    throw new Error(
      `pick exactly one scenario .json (got ${jsons.length}) plus the files it references`,
    );
  }
  const scenario = parseScenario(JSON.parse(await jsons[0].text()));

  // Snapshot every non-JSON file's bytes up front: a File handle can go
  // stale after the picker closes on some platforms, and a Restart must
  // not depend on re-reading disk.
  const byBasename = new Map<string, Uint8Array>();
  for (const f of files) {
    if (f !== jsons[0]) {
      byBasename.set(f.name, new Uint8Array(await f.arrayBuffer()));
    }
  }
  const lookup = (ref: string): Uint8Array => {
    const base = ref.split("/").pop() ?? ref;
    const bytes = byBasename.get(base);
    if (bytes === undefined) {
      throw new Error(`scenario references "${ref}" — pick "${base}" together with the .json`);
    }
    return bytes;
  };
  return {
    scenario,
    io: {
      readBytes: async (ref) => lookup(ref),
      readText: async (ref) => new TextDecoder().decode(lookup(ref)),
      getDspBytes,
    },
  };
}

/**
 * Owns the active playback: the override providers, the timeline cursor, and
 * the loaded scenario. One rearm() per SimHost activation — providers and
 * the timeline hold per-run state (DSP flux history, samplesPcm position,
 * fired events), and the host restart resets the sim clock to 0, so both
 * sides of the clock agree on "frame 0" (same reason the CLI refuses
 * --start-time-ms for finite-stimulus scenarios).
 */
export class ScenarioPlayer {
  private loaded: LoadedScenario | null = null;
  private timeline: TimelineRunner | null = null;

  audioProvider: AudioFeatureProvider | null = null;
  imuProvider: ImuProvider | null = null;

  get active(): boolean {
    return this.loaded !== null;
  }

  get scenario(): Scenario | null {
    return this.loaded?.scenario ?? null;
  }

  finished(simTimeMs: number): boolean {
    return this.loaded !== null && simTimeMs >= this.loaded.scenario.durationMs;
  }

  /** Marks the scenario as the active one. Providers are built by rearm()
   * — called by the activation path — so every host activation (Play,
   * Restart, extension switch/Reload mid-scenario) replays from frame 0
   * with fresh state. */
  setLoaded(loaded: LoadedScenario): void {
    this.loaded = loaded;
  }

  /** (Re)builds providers + timeline for a fresh host. Throws with an
   * actionable message when a referenced file or the DSP is unavailable. */
  async rearm(): Promise<void> {
    if (this.loaded === null) {
      return;
    }
    const { scenario, io } = this.loaded;
    // Build both before publishing either, so a failure leaves the previous
    // (still-consistent) pair in place rather than half a scenario.
    const audio = await buildAudioProvider(scenario.audio, {
      scenario,
      seed: scenario.seed ?? 0,
      io,
    });
    const imu = buildImuProvider(scenario.imu, scenario.durationMs);
    this.audioProvider = audio;
    this.imuProvider = imu;
    this.timeline = new TimelineRunner(scenario.timeline);
  }

  /** Fires due timeline events. Throws on a bad param name/value (surface
   * it and stop the scenario — the file disagrees with the extension). */
  pump(host: SimHost): { firedSet: boolean; firedPress: boolean } {
    if (this.timeline === null) {
      return { firedSet: false, firedPress: false };
    }
    return this.timeline.pump(host);
  }

  /** Back to manual inputs. The tap providers fall back as soon as the
   * override fields are null; no host restart needed. */
  stop(): void {
    this.loaded = null;
    this.timeline = null;
    this.audioProvider = null;
    this.imuProvider = null;
  }
}
