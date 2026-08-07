/**
 * D-line frame codec — the audio tap text format defined by
 * fw/src/sound/audio_tap_format.h (see that header for the grammar). This
 * is the FOURTH producer/consumer of the format, after sound.cpp, the
 * native_sim replay harness, and fw/tools/beat_lab/frames.py — keep all
 * of them in sync if the format changes.
 *
 *   D,<seq>,<gain hex2>,<beatmask hex1>,<rms>,<e0..e3>,<f0..f3>,<m0..m3>,<s0..s3>[,<d0..d19>]
 *
 * Floats are 8-hex-char IEEE-754 bit patterns (exact round-trip).
 */

import { AudioFeatures } from "../core/providers";
import {
  RGBX_AUDIO_NUM_BANDS,
  RGBX_AUDIO_NUM_DISPLAY_BUCKETS,
} from "../core/abi";

const f32buf = new DataView(new ArrayBuffer(4));

export function f32FromHex(hex: string): number {
  f32buf.setUint32(0, parseInt(hex, 16));
  return f32buf.getFloat32(0);
}

export function f32ToHex(v: number): string {
  f32buf.setFloat32(0, v);
  return f32buf.getUint32(0).toString(16).padStart(8, "0");
}

export interface DLineFrame {
  seq: number;
  gain: number;
  rms: number;
  bandEnergy: Float32Array;
  bandFlux: Float32Array;
  bandMean: Float32Array;
  bandSigma: Float32Array;
  beat: Uint8Array;
  /** Empty when the producer omitted buckets (pre-bucket firmware dumps). */
  displayBucket: Float32Array;
}

/** Parses every D-line in a dump; ignores #PARAMS/#DONE/other lines. */
export function parseDLines(text: string): DLineFrame[] {
  const frames: DLineFrame[] = [];
  for (const line of text.split("\n")) {
    if (!line.startsWith("D,")) {
      continue;
    }
    const parts = line.trim().split(",");
    // D + seq + gain + beatmask + rms + 16 band floats (+ 20 buckets)
    if (parts.length !== 21 && parts.length !== 41) {
      continue; // tolerate truncated capture tails, like frames.py does
    }
    const beatmask = parseInt(parts[3], 16);
    const beat = new Uint8Array(RGBX_AUDIO_NUM_BANDS);
    for (let b = 0; b < RGBX_AUDIO_NUM_BANDS; b++) {
      beat[b] = (beatmask >> b) & 1;
    }
    const floats = (offset: number, n: number): Float32Array => {
      const out = new Float32Array(n);
      for (let i = 0; i < n; i++) {
        out[i] = f32FromHex(parts[offset + i]);
      }
      return out;
    };
    frames.push({
      seq: parseInt(parts[1], 10),
      gain: parseInt(parts[2], 16),
      rms: f32FromHex(parts[4]),
      bandEnergy: floats(5, 4),
      bandFlux: floats(9, 4),
      bandMean: floats(13, 4),
      bandSigma: floats(17, 4),
      beat,
      displayBucket:
        parts.length === 41
          ? floats(21, RGBX_AUDIO_NUM_DISPLAY_BUCKETS)
          : new Float32Array(RGBX_AUDIO_NUM_DISPLAY_BUCKETS),
    });
  }
  return frames;
}

export function dLineFramesToFeatures(frames: DLineFrame[]): AudioFeatures[] {
  return frames.map((f) => ({
    bandEnergy: f.bandEnergy,
    beat: f.beat,
    displayBucket: f.displayBucket,
  }));
}

/** Emits one D-line (no newline) from a full DSP frame — used by
 * `rgbx-sim dsp-replay` for parity against the native_sim harness. */
export function formatDLine(frame: {
  seq: number;
  gain?: number;
  rms?: number;
  bandEnergy: Float32Array;
  bandFlux: Float32Array;
  bandMean: Float32Array;
  bandSigma: Float32Array;
  beat: Uint8Array;
  displayBucket: Float32Array;
}): string {
  let beatmask = 0;
  for (let b = 0; b < RGBX_AUDIO_NUM_BANDS; b++) {
    if (frame.beat[b] !== 0) {
      beatmask |= 1 << b;
    }
  }
  const parts = [
    "D",
    String(frame.seq),
    (frame.gain ?? 0).toString(16).padStart(2, "0"),
    beatmask.toString(16),
    f32ToHex(frame.rms ?? 0),
  ];
  for (const arr of [frame.bandEnergy, frame.bandFlux, frame.bandMean, frame.bandSigma]) {
    for (let b = 0; b < RGBX_AUDIO_NUM_BANDS; b++) {
      parts.push(f32ToHex(arr[b]));
    }
  }
  for (let i = 0; i < RGBX_AUDIO_NUM_DISPLAY_BUCKETS; i++) {
    parts.push(f32ToHex(frame.displayBucket[i]));
  }
  return parts.join(",");
}
