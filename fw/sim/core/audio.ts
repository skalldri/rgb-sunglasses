/**
 * DspAudioProvider — AudioFeatureProvider backed by the REAL firmware DSP
 * compiled to WASM (out/wasm/audio_dsp.wasm). Each 32 ms sim-time boundary
 * pushes one 512-sample PCM block through audio_dsp_process(); the returned
 * band energies / beat flags / display buckets are bit-for-bit what the
 * firmware pipeline computes for the same samples (modulo float platform
 * differences — see PARITY.md).
 */

import {
  RGBX_AUDIO_NUM_BANDS,
  RGBX_AUDIO_NUM_DISPLAY_BUCKETS,
} from "./abi";
import { AUDIO_FRAME_SAMPLES, AudioFeatureProvider, AudioFeatures } from "./providers";
import type { PcmGenerator } from "./pcmGen";

interface DspExports {
  memory: WebAssembly.Memory;
  _initialize?: () => void;
  sim_init: () => void;
  sim_process: (seq: number) => void;
  sim_pcm: WebAssembly.Global;
  sim_band_energy: WebAssembly.Global;
  sim_band_flux: WebAssembly.Global;
  sim_band_mean: WebAssembly.Global;
  sim_band_sigma: WebAssembly.Global;
  sim_beat: WebAssembly.Global;
  sim_display_bucket: WebAssembly.Global;
}

export interface DspFrameTap {
  /** Called after each processed frame with the full DSP result (incl.
   * flux/mean/sigma, which the rgbx ABI does NOT expose) — used by the
   * D-line dump for parity testing. */
  (frame: {
    seq: number;
    bandEnergy: Float32Array;
    bandFlux: Float32Array;
    bandMean: Float32Array;
    bandSigma: Float32Array;
    beat: Uint8Array;
    displayBucket: Float32Array;
  }): void;
}

export class DspAudioProvider implements AudioFeatureProvider {
  private constructor(
    private readonly ex: DspExports,
    private pcm: PcmGenerator,
    private readonly tap: DspFrameTap | null,
  ) {}

  static async create(
    dspWasmBytes: ArrayBuffer,
    pcm: PcmGenerator,
    tap: DspFrameTap | null = null,
  ): Promise<DspAudioProvider> {
    const { instance } = await WebAssembly.instantiate(dspWasmBytes, {});
    const ex = instance.exports as unknown as DspExports;
    ex._initialize?.();
    ex.sim_init();
    return new DspAudioProvider(ex, pcm, tap);
  }

  /** Swap the PCM source mid-run (browser UI: mic <-> WAV <-> generator). */
  setPcmGenerator(pcm: PcmGenerator): void {
    this.pcm = pcm;
  }

  nextFrame(frameIndex: number): AudioFeatures {
    const block = this.pcm(frameIndex);
    const mem = this.ex.memory.buffer;
    new Int16Array(mem, this.ex.sim_pcm.value as number, AUDIO_FRAME_SAMPLES).set(block);
    this.ex.sim_process(frameIndex);

    // Snapshot (the wasm views alias linear memory; callers keep frames).
    const bandEnergy = new Float32Array(
      mem, this.ex.sim_band_energy.value as number, RGBX_AUDIO_NUM_BANDS).slice();
    const beat = new Uint8Array(
      mem, this.ex.sim_beat.value as number, RGBX_AUDIO_NUM_BANDS).slice();
    const displayBucket = new Float32Array(
      mem, this.ex.sim_display_bucket.value as number, RGBX_AUDIO_NUM_DISPLAY_BUCKETS).slice();

    if (this.tap !== null) {
      this.tap({
        seq: frameIndex,
        bandEnergy,
        bandFlux: new Float32Array(mem, this.ex.sim_band_flux.value as number, RGBX_AUDIO_NUM_BANDS).slice(),
        bandMean: new Float32Array(mem, this.ex.sim_band_mean.value as number, RGBX_AUDIO_NUM_BANDS).slice(),
        bandSigma: new Float32Array(mem, this.ex.sim_band_sigma.value as number, RGBX_AUDIO_NUM_BANDS).slice(),
        beat,
        displayBucket,
      });
    }

    return { bandEnergy, beat, displayBucket };
  }
}
