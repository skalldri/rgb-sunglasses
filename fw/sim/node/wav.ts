/**
 * Minimal WAV reader for scenario audio: PCM16 / PCM8 / float32, any
 * channel count (averaged to mono), linear-interpolation resample to the
 * device's 16 kHz. No dependencies.
 */

import { AUDIO_SAMPLE_RATE } from "../core/providers";

export function decodeWavTo16kMono(bytes: Buffer): Int16Array {
  if (bytes.length < 44 || bytes.toString("ascii", 0, 4) !== "RIFF" || bytes.toString("ascii", 8, 12) !== "WAVE") {
    throw new Error("not a RIFF/WAVE file");
  }

  let fmt: { format: number; channels: number; sampleRate: number; bitsPerSample: number } | null = null;
  let dataOffset = -1;
  let dataLength = 0;

  let at = 12;
  while (at + 8 <= bytes.length) {
    const id = bytes.toString("ascii", at, at + 4);
    const size = bytes.readUInt32LE(at + 4);
    const body = at + 8;
    if (id === "fmt ") {
      fmt = {
        format: bytes.readUInt16LE(body),
        channels: bytes.readUInt16LE(body + 2),
        sampleRate: bytes.readUInt32LE(body + 4),
        bitsPerSample: bytes.readUInt16LE(body + 14),
      };
    } else if (id === "data") {
      dataOffset = body;
      dataLength = Math.min(size, bytes.length - body);
    }
    at = body + size + (size % 2); // chunks are word-aligned
  }
  if (fmt === null || dataOffset < 0) {
    throw new Error("missing fmt/data chunk");
  }
  const { format, channels, sampleRate, bitsPerSample } = fmt;

  // Fast path: an already-conformant WAV (16-bit PCM, mono, 16 kHz — e.g. a
  // device `record_wav` capture) passes through BYTE-EXACT. This matters for
  // the DSP parity gate: the native_sim replay harness reads the same file's
  // raw int16 samples, and any float round-trip here (even a symmetric one)
  // would make compare_sim.py measure input skew instead of build parity.
  if (format === 1 && bitsPerSample === 16 && channels === 1 && sampleRate === AUDIO_SAMPLE_RATE) {
    const frames16 = Math.floor(dataLength / 2);
    const out = new Int16Array(frames16);
    for (let i = 0; i < frames16; i++) {
      out[i] = bytes.readInt16LE(dataOffset + i * 2);
    }
    return out;
  }

  // Decode to mono float in [-1, 1). The scale is 32768 on BOTH sides of
  // the conversion (with a clamp on re-encode) so int16 values that survive
  // mixing/resampling untouched round-trip exactly.
  let frames: number;
  let read: (frame: number, ch: number) => number;
  if (format === 1 && bitsPerSample === 16) {
    frames = Math.floor(dataLength / (2 * channels));
    read = (f, c) => bytes.readInt16LE(dataOffset + (f * channels + c) * 2) / 32768;
  } else if (format === 1 && bitsPerSample === 8) {
    frames = Math.floor(dataLength / channels);
    read = (f, c) => (bytes.readUInt8(dataOffset + f * channels + c) - 128) / 128;
  } else if (format === 3 && bitsPerSample === 32) {
    frames = Math.floor(dataLength / (4 * channels));
    read = (f, c) => bytes.readFloatLE(dataOffset + (f * channels + c) * 4);
  } else {
    throw new Error(`unsupported WAV: format=${format} bits=${bitsPerSample}`);
  }

  const mono = new Float32Array(frames);
  for (let f = 0; f < frames; f++) {
    let sum = 0;
    for (let c = 0; c < channels; c++) {
      sum += read(f, c);
    }
    mono[f] = sum / channels;
  }

  // Resample to 16 kHz (linear interpolation — fine for a stimulus; device
  // recordings are already 16 kHz and skip this path entirely).
  let resampled: Float32Array;
  if (sampleRate === AUDIO_SAMPLE_RATE) {
    resampled = mono;
  } else {
    const outLen = Math.floor((frames * AUDIO_SAMPLE_RATE) / sampleRate);
    resampled = new Float32Array(outLen);
    for (let i = 0; i < outLen; i++) {
      const src = (i * sampleRate) / AUDIO_SAMPLE_RATE;
      const i0 = Math.floor(src);
      const i1 = Math.min(i0 + 1, frames - 1);
      const t = src - i0;
      resampled[i] = mono[i0] * (1 - t) + mono[i1] * t;
    }
  }

  const out = new Int16Array(resampled.length);
  for (let i = 0; i < resampled.length; i++) {
    out[i] = Math.max(-32768, Math.min(32767, Math.round(resampled[i] * 32768)));
  }
  return out;
}
