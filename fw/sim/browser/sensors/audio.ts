/**
 * Audio input plumbing for the browser UI.
 *
 * Every source — generators, decoded files, live mic — is fed to the SAME
 * DspAudioProvider, i.e. through the real firmware DSP compiled to wasm, so
 * band energies and beat flags are the genuine algorithm's output rather
 * than a UI approximation. Switching sources only swaps the PCM generator;
 * the DSP's own history (flux means/sigmas) deliberately carries over, just
 * as it would on the device when the room's audio changes.
 *
 * If audio_dsp.wasm can't be fetched the manager degrades to silence rather
 * than failing the page — an extension that ignores audio is still usable.
 */

import { DspAudioProvider } from "../../core/audio";
// ?worker&url makes Vite BUILD the worklet as its own transpiled chunk and
// hand back its URL — the only pattern that survives `vite build` (a plain
// new URL(...) reference would emit the raw .ts as an asset, breaking the
// mic on the deployed /sim/ page; found via browser/smoke.md's limitation).
import micWorkletUrl from "./mic-worklet?worker&url";
import {
  AUDIO_FRAME_MS,
  AUDIO_FRAME_SAMPLES,
  AUDIO_SAMPLE_RATE,
  AudioFeatureProvider,
  AudioFeatures,
  SilenceAudioProvider,
  zeroAudioFeatures,
} from "../../core/providers";
import {
  PcmGenerator,
  metronomePcm,
  noisePcm,
  samplesPcm,
  silencePcm,
  sweepPcm,
} from "../../core/pcmGen";

export type AudioSourceKind = "silence" | "metronome" | "sweep" | "noise" | "file" | "mic";

export interface AudioSettings {
  kind: AudioSourceKind;
  /** Metronome tempo, 60..200. */
  bpm: number;
  /** Generator level in dBFS (metronome / sweep / noise). */
  gainDb: number;
}

const SILENT_BLOCK = new Int16Array(AUDIO_FRAME_SAMPLES);

export class AudioSources {
  private dsp: DspAudioProvider | null = null;
  private readonly silence = new SilenceAudioProvider();

  readonly settings: AudioSettings = { kind: "silence", bpm: 120, gainDb: -6 };

  private fileSamples: Int16Array | null = null;
  private fileName = "";

  private micBlock: Int16Array | null = null;
  private micRms = 0;
  private micCtx: AudioContext | null = null;
  private micStream: MediaStream | null = null;

  /** Audio-frame index at which the current source was selected, so each
   * source begins at its own frame 0 instead of jumping into the middle of
   * a clip / metronome bar. */
  private originFrame = 0;
  /** Supplied by the run loop: the sim's current audio-frame index. */
  private frameClock: () => number = () => 0;

  setFrameClock(fn: () => number): void {
    this.frameClock = fn;
  }

  /** The provider to assign to SimHost.audioProvider. Stable across source
   * changes once the DSP has loaded. */
  get provider(): AudioFeatureProvider {
    return this.dsp ?? this.silence;
  }

  get dspReady(): boolean {
    return this.dsp !== null;
  }

  get loadedFileName(): string {
    return this.fileName;
  }

  /** Peak-ish mic level in [0, 1] for the level meter. */
  get micLevel(): number {
    return this.micRms;
  }

  get micActive(): boolean {
    return this.micCtx !== null;
  }

  /** Fetches and instantiates the firmware DSP. Returns false (and leaves
   * the manager on silence) if the artifact isn't built or fails to load. */
  async loadDsp(url = `${import.meta.env.BASE_URL}audio_dsp.wasm`): Promise<boolean> {
    try {
      const resp = await fetch(url);
      if (!resp.ok) {
        return false;
      }
      this.dsp = await DspAudioProvider.create(await resp.arrayBuffer(), silencePcm());
      this.applySource();
      return true;
    } catch {
      return false;
    }
  }

  /** Selects a source and/or updates generator settings. Restarts the
   * source's own clock unless `keepPhase` (used for live slider drags). */
  setSource(patch: Partial<AudioSettings>, keepPhase = false): void {
    const kindChanged = patch.kind !== undefined && patch.kind !== this.settings.kind;
    Object.assign(this.settings, patch);
    if (!keepPhase || kindChanged) {
      this.originFrame = this.frameClock();
    }
    this.applySource();
  }

  /** Decodes an audio file to 16 kHz mono int16 and selects it as the
   * source. Returns the clip duration in seconds. */
  async loadFile(file: File): Promise<number> {
    // decodeAudioData resamples to the context's rate, so decoding straight
    // into a 16 kHz OfflineAudioContext does the resampling for us. The
    // context length is irrelevant to decoding — only its sampleRate is.
    const ctx = new OfflineAudioContext(1, 1, AUDIO_SAMPLE_RATE);
    const decoded = await ctx.decodeAudioData(await file.arrayBuffer());

    const n = decoded.length;
    const mix = new Float32Array(n);
    for (let ch = 0; ch < decoded.numberOfChannels; ch++) {
      const data = decoded.getChannelData(ch);
      for (let i = 0; i < n; i++) {
        mix[i] += data[i];
      }
    }
    const scale = 32767 / Math.max(1, decoded.numberOfChannels);
    const samples = new Int16Array(n);
    for (let i = 0; i < n; i++) {
      const v = Math.round(mix[i] * scale);
      samples[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }

    this.fileSamples = samples;
    this.fileName = file.name;
    this.setSource({ kind: "file" });
    return n / AUDIO_SAMPLE_RATE;
  }

  /** Opens the microphone and starts the capture worklet. Throws on denial
   * or when the page isn't in a secure context. */
  async startMic(): Promise<void> {
    if (this.micCtx !== null) {
      return;
    }
    if (navigator.mediaDevices?.getUserMedia === undefined) {
      throw new Error("getUserMedia unavailable — the page needs a secure context (https or localhost)");
    }
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: {
        echoCancellation: false,
        autoGainControl: false,
        noiseSuppression: false,
      },
    });

    // Ask for the DSP's native rate; browsers that refuse fall back to their
    // own rate and the worklet resamples.
    let ctx: AudioContext;
    try {
      ctx = new AudioContext({ sampleRate: AUDIO_SAMPLE_RATE });
    } catch {
      ctx = new AudioContext();
    }
    await ctx.audioWorklet.addModule(micWorkletUrl);
    const node = new AudioWorkletNode(ctx, "rgbx-mic-capture", {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      processorOptions: { targetRate: AUDIO_SAMPLE_RATE, blockSize: AUDIO_FRAME_SAMPLES },
    });
    node.port.onmessage = (ev: MessageEvent<Int16Array>) => {
      this.micBlock = ev.data;
      this.micRms = rms(ev.data);
    };

    // A worklet node is only pulled while it is connected to the graph;
    // route it through a muted gain so nothing is audible.
    const mute = ctx.createGain();
    mute.gain.value = 0;
    ctx.createMediaStreamSource(stream).connect(node);
    node.connect(mute).connect(ctx.destination);
    await ctx.resume();

    this.micCtx = ctx;
    this.micStream = stream;
    this.setSource({ kind: "mic" });
  }

  stopMic(): void {
    this.micStream?.getTracks().forEach((t) => t.stop());
    void this.micCtx?.close();
    this.micCtx = null;
    this.micStream = null;
    this.micBlock = null;
    this.micRms = 0;
    if (this.settings.kind === "mic") {
      this.setSource({ kind: "silence" });
    }
  }

  private applySource(): void {
    this.dsp?.setPcmGenerator(this.buildGenerator());
  }

  private buildGenerator(): PcmGenerator {
    const { kind, bpm, gainDb } = this.settings;
    if (kind === "mic") {
      // Live capture is inherently non-deterministic and has no frame
      // clock of its own — always the most recent captured block.
      return () => this.micBlock ?? SILENT_BLOCK;
    }

    let base: PcmGenerator;
    switch (kind) {
      case "metronome":
        base = metronomePcm({ bpm, gainDb });
        break;
      case "sweep":
        base = sweepPcm({ fromHz: 60, toHz: 6000, durationMs: 8000, gainDb });
        break;
      case "noise":
        base = noisePcm({ color: "pink", gainDb });
        break;
      case "file":
        base = this.fileSamples === null ? silencePcm() : samplesPcm(this.fileSamples);
        break;
      default:
        base = silencePcm();
        break;
    }
    const origin = this.originFrame;
    return (frameIndex) => base(Math.max(0, frameIndex - origin));
  }
}

/**
 * Pass-through provider that remembers the last frame it handed to the host,
 * so the UI can show live band energies and beat flags. It also indirects
 * through a getter, which lets it be assigned to SimHost.audioProvider once
 * at construction and still pick up the real DSP provider whenever
 * audio_dsp.wasm finishes loading.
 */
export class TappedAudioProvider implements AudioFeatureProvider {
  last: AudioFeatures = zeroAudioFeatures();

  constructor(private readonly inner: () => AudioFeatureProvider) {}

  async nextFrame(frameIndex: number): Promise<AudioFeatures> {
    const frame = await this.inner().nextFrame(frameIndex);
    this.last = frame;
    return frame;
  }
}

/** Converts sim time to the audio-frame index the host is about to request. */
export function frameIndexForSimTime(simTimeMs: number): number {
  return Math.floor(simTimeMs / AUDIO_FRAME_MS);
}

function rms(block: Int16Array): number {
  let sum = 0;
  for (let i = 0; i < block.length; i++) {
    const v = block[i] / 32768;
    sum += v * v;
  }
  return Math.sqrt(sum / Math.max(1, block.length));
}
