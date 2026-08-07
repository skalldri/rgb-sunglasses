/**
 * Deterministic 16 kHz mono PCM generators for scenario audio. Each returns
 * a function producing the next 512-sample block (one 32 ms audio frame) —
 * fed through the REAL firmware DSP (audio_dsp.wasm) so beats and band
 * energies come from the genuine algorithm.
 */

import { AUDIO_FRAME_SAMPLES, AUDIO_SAMPLE_RATE } from "./providers";
import { mulberry32 } from "./rng";

export type PcmGenerator = (frameIndex: number) => Int16Array;

function dbToAmplitude(gainDb: number): number {
  return 32767 * Math.pow(10, gainDb / 20);
}

export function silencePcm(): PcmGenerator {
  const block = new Int16Array(AUDIO_FRAME_SAMPLES);
  return () => block;
}

/** Click train at `bpm`: a `clickMs`-long `clickHz` tone burst per beat,
 * with an exponential decay envelope so the onset is sharp (what the
 * spectral-flux detector keys on). */
export function metronomePcm(opts: {
  bpm: number;
  clickHz?: number;
  clickMs?: number;
  gainDb?: number;
}): PcmGenerator {
  const clickHz = opts.clickHz ?? 1000;
  const clickMs = opts.clickMs ?? 20;
  const amp = dbToAmplitude(opts.gainDb ?? -6);
  const beatPeriodSamples = Math.round((60 / opts.bpm) * AUDIO_SAMPLE_RATE);
  const clickSamples = Math.round((clickMs / 1000) * AUDIO_SAMPLE_RATE);
  return (frameIndex) => {
    const block = new Int16Array(AUDIO_FRAME_SAMPLES);
    const base = frameIndex * AUDIO_FRAME_SAMPLES;
    for (let i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
      const t = base + i;
      const sinceBeat = t % beatPeriodSamples;
      if (sinceBeat < clickSamples) {
        const envelope = Math.exp((-4 * sinceBeat) / clickSamples);
        block[i] = Math.round(
          amp * envelope * Math.sin((2 * Math.PI * clickHz * sinceBeat) / AUDIO_SAMPLE_RATE),
        );
      }
    }
    return block;
  };
}

/** Linear frequency sweep fromHz -> toHz over durationMs, then holds toHz. */
export function sweepPcm(opts: {
  fromHz: number;
  toHz: number;
  durationMs: number;
  gainDb?: number;
}): PcmGenerator {
  const amp = dbToAmplitude(opts.gainDb ?? -12);
  const durationSamples = (opts.durationMs / 1000) * AUDIO_SAMPLE_RATE;
  let phase = 0;
  return (frameIndex) => {
    const block = new Int16Array(AUDIO_FRAME_SAMPLES);
    const base = frameIndex * AUDIO_FRAME_SAMPLES;
    for (let i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
      const t = base + i;
      const progress = Math.min(t / durationSamples, 1);
      const hz = opts.fromHz + (opts.toHz - opts.fromHz) * progress;
      phase += (2 * Math.PI * hz) / AUDIO_SAMPLE_RATE;
      block[i] = Math.round(amp * Math.sin(phase));
    }
    return block;
  };
}

/** Seeded white/pink noise. Pink via the Voss-McCartney-ish one-pole
 * cascade — close enough for a stimulus; determinism is what matters. */
export function noisePcm(opts: { color?: "white" | "pink"; gainDb?: number; seed?: number }): PcmGenerator {
  const amp = dbToAmplitude(opts.gainDb ?? -18);
  const rng = mulberry32(opts.seed ?? 1234);
  const rand = () => rng() / 0x100000000 - 0.5; // [-0.5, 0.5)
  let b0 = 0;
  let b1 = 0;
  let b2 = 0;
  const pink = opts.color === "pink";
  return () => {
    const block = new Int16Array(AUDIO_FRAME_SAMPLES);
    for (let i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
      const white = rand();
      let sample = white;
      if (pink) {
        b0 = 0.99765 * b0 + white * 0.099046;
        b1 = 0.963 * b1 + white * 0.2965164;
        b2 = 0.57 * b2 + white * 1.0526913;
        sample = (b0 + b1 + b2 + white * 0.1848) * 0.25;
      }
      block[i] = Math.max(-32768, Math.min(32767, Math.round(2 * amp * sample)));
    }
    return block;
  };
}

/** Wraps raw 16 kHz mono int16 samples (e.g. a decoded WAV) into 512-sample
 * blocks; silence past the end. */
export function samplesPcm(samples: Int16Array): PcmGenerator {
  return (frameIndex) => {
    const block = new Int16Array(AUDIO_FRAME_SAMPLES);
    const base = frameIndex * AUDIO_FRAME_SAMPLES;
    if (base < samples.length) {
      block.set(samples.subarray(base, Math.min(base + AUDIO_FRAME_SAMPLES, samples.length)));
    }
    return block;
  };
}
