/**
 * Builds the audio/IMU providers a scenario asks for — the platform-agnostic
 * half of scenario execution, shared by the Node CLI (node/scenarioRun.ts)
 * and the browser scenario player (browser/scenario.ts). File access is the
 * only platform-specific part, injected via ScenarioIo: the CLI resolves
 * `file:` refs against the scenario's directory with fs, the browser fetches
 * them relative to the scenario's URL (or a picked local file set).
 */

import { DspAudioProvider } from "./audio";
import { dLineFramesToFeatures, parseDLines } from "./dline";
import { KeyframeImuProvider, RampImuProvider, SineImuProvider } from "./imuGen";
import { metronomePcm, noisePcm, samplesPcm, sweepPcm } from "./pcmGen";
import {
  AudioFeatureProvider,
  FeatureReplayProvider,
  ImuProvider,
  SilenceAudioProvider,
  StaticImuProvider,
} from "./providers";
import { Scenario, ScenarioAudio, ScenarioImu } from "./scenario";
import { decodeWavTo16kMono } from "./wav";

/** Platform I/O a scenario needs: `file:` refs (resolved scenario-relative)
 * and the real firmware DSP wasm for PCM-synthesizing audio types. */
export interface ScenarioIo {
  readBytes(ref: string): Promise<Uint8Array>;
  readText(ref: string): Promise<string>;
  /** audio_dsp.wasm bytes. Throw an actionable error when unavailable (not
   * built / not bundled) — buildAudioProvider adds the audio-type context. */
  getDspBytes(): Promise<ArrayBuffer>;
}

export async function buildAudioProvider(
  audio: ScenarioAudio | undefined,
  opts: { scenario: Scenario; seed: number; io: ScenarioIo },
): Promise<AudioFeatureProvider> {
  if (audio === undefined || audio.type === "silence") {
    return new SilenceAudioProvider();
  }
  if (audio.type === "features") {
    const text = await opts.io.readText(audio.file);
    return new FeatureReplayProvider(dLineFramesToFeatures(parseDLines(text)));
  }
  // All remaining types synthesize PCM and need the real DSP module.
  let dspBytes: ArrayBuffer;
  try {
    dspBytes = await opts.io.getDspBytes();
  } catch (err) {
    const detail = err instanceof Error ? err.message : String(err);
    throw new Error(`${detail} — required for audio type "${audio.type}"`);
  }
  switch (audio.type) {
    case "metronome":
      return DspAudioProvider.create(dspBytes, metronomePcm(audio));
    case "sweep":
      return DspAudioProvider.create(
        dspBytes,
        sweepPcm({ ...audio, durationMs: audio.durationMs ?? opts.scenario.durationMs }),
      );
    case "noise":
      return DspAudioProvider.create(
        dspBytes,
        noisePcm({ ...audio, seed: audio.seed ?? opts.seed }),
      );
    case "wav": {
      const samples = decodeWavTo16kMono(await opts.io.readBytes(audio.file));
      return DspAudioProvider.create(dspBytes, samplesPcm(samples));
    }
  }
}

export function buildImuProvider(imu: ScenarioImu | undefined, durationMs: number): ImuProvider {
  if (imu === undefined) {
    return new StaticImuProvider();
  }
  switch (imu.type) {
    case "static":
      return new StaticImuProvider({
        accel: imu.accel ?? [0, 0, 9.81],
        gyro: imu.gyro ?? [0, 0, 0],
      });
    case "ramp":
      return new RampImuProvider({
        fromAccel: imu.fromAccel,
        toAccel: imu.toAccel,
        fromGyro: imu.fromGyro,
        toGyro: imu.toGyro,
        startMs: imu.startMs ?? 0,
        endMs: imu.endMs ?? durationMs,
      });
    case "sine":
      return new SineImuProvider({
        channel: imu.channel ?? "accel",
        axis: imu.axis,
        amplitude: imu.amplitude,
        hz: imu.hz,
        base: imu.base
          ? { accel: imu.base.accel, gyro: imu.base.gyro ?? [0, 0, 0] }
          : undefined,
      });
    case "keyframes":
      return new KeyframeImuProvider(imu.frames);
  }
}
