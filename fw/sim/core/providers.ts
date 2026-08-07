/**
 * Input source interfaces + basic built-in providers.
 *
 * Cadence contract (matches the device, see PARITY.md):
 *  - Audio features refresh at 31.25 Hz (one 512-sample block per 32 ms of
 *    sim time) and are STICKY between refreshes — including beat flags,
 *    which therefore repeat across ~3 render ticks exactly like the
 *    device's drain-and-cache adapter.
 *  - IMU samples refresh at 25 Hz (40 ms) and hold between refreshes.
 */

import { RGBX_AUDIO_NUM_BANDS, RGBX_AUDIO_NUM_DISPLAY_BUCKETS } from "./abi";

export const AUDIO_FRAME_MS = 32; // 512 samples @ 16 kHz
export const AUDIO_FRAME_SAMPLES = 512;
export const AUDIO_SAMPLE_RATE = 16000;
export const IMU_PERIOD_MS = 40; // BMI270 @ 25 Hz

export interface AudioFeatures {
  bandEnergy: Float32Array; // 4, raw mean power (NOT normalized)
  beat: Uint8Array; // 4
  displayBucket: Float32Array; // 20, raw mean power
}

export function zeroAudioFeatures(): AudioFeatures {
  return {
    bandEnergy: new Float32Array(RGBX_AUDIO_NUM_BANDS),
    beat: new Uint8Array(RGBX_AUDIO_NUM_BANDS),
    displayBucket: new Float32Array(RGBX_AUDIO_NUM_DISPLAY_BUCKETS),
  };
}

/** Delivers one feature frame per 32 ms audio block. Implementations:
 * silence, D-line feature replay, PCM-through-real-DSP (audio.ts). */
export interface AudioFeatureProvider {
  /** Called once per 32 ms audio-frame boundary, in order. `frameIndex`
   * counts from 0. May be async (the DSP provider round-trips a wasm call). */
  nextFrame(frameIndex: number): AudioFeatures | Promise<AudioFeatures>;
}

export class SilenceAudioProvider implements AudioFeatureProvider {
  nextFrame(): AudioFeatures {
    return zeroAudioFeatures();
  }
}

/** Replays pre-computed feature frames (from a device `sound dump` or the
 * native_sim replay harness); holds the last frame when the list runs out. */
export class FeatureReplayProvider implements AudioFeatureProvider {
  constructor(private readonly frames: AudioFeatures[]) {}
  nextFrame(frameIndex: number): AudioFeatures {
    if (this.frames.length === 0) {
      return zeroAudioFeatures();
    }
    return this.frames[Math.min(frameIndex, this.frames.length - 1)];
  }
}

export interface ImuSample {
  accel: [number, number, number]; // m/s^2
  gyro: [number, number, number]; // rad/s
}

export interface ImuProvider {
  /** Returns the sample the IMU would have produced at time tMs. Called on
   * the 40 ms grid; the host holds the value between grid points. */
  sampleAt(tMs: number): ImuSample;
}

export class StaticImuProvider implements ImuProvider {
  constructor(private readonly sample: ImuSample = { accel: [0, 0, 9.81], gyro: [0, 0, 0] }) {}
  sampleAt(): ImuSample {
    return this.sample;
  }
}

/** Shared band-0 beat latch for RandomOnBeat color modes — mirrors the
 * firmware's SoundAnimationAudioSource latch: set when an audio frame with
 * a band-0 beat arrives, cleared by the first consumer. */
export class BeatLatch {
  private latched = false;
  onAudioFrame(features: AudioFeatures): void {
    if (features.beat[0] !== 0) {
      this.latched = true;
    }
  }
  consume(): boolean {
    const value = this.latched;
    this.latched = false;
    return value;
  }
}
