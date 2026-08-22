import { test } from "node:test";
import assert from "node:assert/strict";
import { decodeWavTo16kMono } from "../core/wav";
import { AUDIO_SAMPLE_RATE } from "../core/providers";

/** Builds a minimal RIFF/WAVE buffer: one fmt chunk, one data chunk. */
function makeWav(opts: {
  format: number; // 1 = PCM, 3 = float
  channels: number;
  sampleRate: number;
  bitsPerSample: number;
  data: Uint8Array;
}): Uint8Array {
  const fmtSize = 16;
  const total = 12 + (8 + fmtSize) + (8 + opts.data.length);
  const out = new Uint8Array(total);
  const view = new DataView(out.buffer);
  const ascii = (at: number, s: string) => {
    for (let i = 0; i < s.length; i++) {
      out[at + i] = s.charCodeAt(i);
    }
  };
  ascii(0, "RIFF");
  view.setUint32(4, total - 8, true);
  ascii(8, "WAVE");
  ascii(12, "fmt ");
  view.setUint32(16, fmtSize, true);
  view.setUint16(20, opts.format, true);
  view.setUint16(22, opts.channels, true);
  view.setUint32(24, opts.sampleRate, true);
  const blockAlign = (opts.channels * opts.bitsPerSample) / 8;
  view.setUint32(28, opts.sampleRate * blockAlign, true);
  view.setUint16(32, blockAlign, true);
  view.setUint16(34, opts.bitsPerSample, true);
  ascii(36, "data");
  view.setUint32(40, opts.data.length, true);
  out.set(opts.data, 44);
  return out;
}

function pcm16Bytes(samples: number[]): Uint8Array {
  const out = new Uint8Array(samples.length * 2);
  const view = new DataView(out.buffer);
  samples.forEach((s, i) => view.setInt16(i * 2, s, true));
  return out;
}

test("16-bit mono 16 kHz passes through byte-exact (the DSP parity fast path)", () => {
  // Values chosen to catch sign/scale slips: extremes, ±1, and asymmetry.
  const samples = [0, 1, -1, 32767, -32768, 12345, -12345];
  const wav = makeWav({
    format: 1,
    channels: 1,
    sampleRate: AUDIO_SAMPLE_RATE,
    bitsPerSample: 16,
    data: pcm16Bytes(samples),
  });
  assert.deepEqual(Array.from(decodeWavTo16kMono(wav)), samples);
});

test("fast path survives a non-zero byteOffset view (Buffer.slice territory)", () => {
  const samples = [100, -200, 300];
  const wav = makeWav({
    format: 1,
    channels: 1,
    sampleRate: AUDIO_SAMPLE_RATE,
    bitsPerSample: 16,
    data: pcm16Bytes(samples),
  });
  // Embed at an odd offset in a larger buffer so bytes.byteOffset != 0.
  const padded = new Uint8Array(wav.length + 7);
  padded.set(wav, 3);
  const view = new Uint8Array(padded.buffer, 3, wav.length);
  assert.deepEqual(Array.from(decodeWavTo16kMono(view)), samples);
});

test("stereo PCM16 averages to mono", () => {
  const wav = makeWav({
    format: 1,
    channels: 2,
    sampleRate: AUDIO_SAMPLE_RATE,
    bitsPerSample: 16,
    data: pcm16Bytes([1000, 3000, -2000, -4000]),
  });
  assert.deepEqual(Array.from(decodeWavTo16kMono(wav)), [2000, -3000]);
});

test("float32 decodes with the 32768 scale and clamps at full scale", () => {
  const floats = new Float32Array([0, 0.5, -0.5, 1.0, -1.0]);
  const wav = makeWav({
    format: 3,
    channels: 1,
    sampleRate: AUDIO_SAMPLE_RATE,
    bitsPerSample: 32,
    data: new Uint8Array(floats.buffer.slice(0)),
  });
  // 1.0 * 32768 clamps to 32767; -1.0 hits -32768 exactly.
  assert.deepEqual(Array.from(decodeWavTo16kMono(wav)), [0, 16384, -16384, 32767, -32768]);
});

test("resamples 8 kHz to 16 kHz by linear interpolation", () => {
  const wav = makeWav({
    format: 1,
    channels: 1,
    sampleRate: 8000,
    bitsPerSample: 16,
    data: pcm16Bytes([0, 1000]),
  });
  const out = decodeWavTo16kMono(wav);
  assert.equal(out.length, 4);
  assert.deepEqual(Array.from(out), [0, 500, 1000, 1000]);
});

test("rejects non-WAV bytes and missing chunks", () => {
  assert.throws(() => decodeWavTo16kMono(new TextEncoder().encode("not a wav")), /RIFF/);
  const noData = makeWav({
    format: 1,
    channels: 1,
    sampleRate: AUDIO_SAMPLE_RATE,
    bitsPerSample: 16,
    data: pcm16Bytes([]),
  });
  // Corrupt the data chunk id so only fmt is found (buffer stays >= 44 bytes).
  noData.set(new TextEncoder().encode("junk"), 36);
  assert.throws(() => decodeWavTo16kMono(noData), /missing fmt\/data/);
});
