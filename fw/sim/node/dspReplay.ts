/**
 * `rgbx-sim dsp-replay` — runs a WAV through the wasm build of the real
 * firmware DSP and emits D-lines (the audio-tap text format from
 * fw/src/sound/audio_tap_format.h). Output is diffable against the
 * native_sim replay harness (fw/tests/sound/audio_dsp_replay/ via
 * fw/tools/beat_lab/replay.py) for the same WAV — that diff IS the
 * sim-vs-firmware audio parity test (fw/tools/beat_lab/compare_sim.py).
 *
 * The rms column is the block RMS in the [-1,1) sample domain (matching
 * the replay harness); the gain column is fixed at 0 — the sim has no AGC
 * loop (browser/WAV input levels are already line-level).
 */

import * as fs from "node:fs";
import * as path from "node:path";
import { DspAudioProvider } from "../core/audio";
import { samplesPcm } from "../core/pcmGen";
import { AUDIO_FRAME_SAMPLES } from "../core/providers";
import { decodeWavTo16kMono } from "./wav";
import { formatDLine } from "./dline";

interface Flags {
  positional: string[];
  options: Map<string, string[]>;
  bools: Set<string>;
}

export async function dspReplay(
  flags: Flags,
  fail: (msg: string) => never,
  wasmDir: string,
): Promise<void> {
  const wavPath = flags.options.get("wav")?.[0];
  if (wavPath === undefined) {
    fail("dsp-replay needs --wav <file>");
  }
  const outPath = flags.options.get("out")?.[0] ?? null;

  const dspPath = path.join(wasmDir, "audio_dsp.wasm");
  if (!fs.existsSync(dspPath)) {
    fail(`${dspPath} not built — run fw/sim/build-extensions.sh`);
  }
  const dspBuf = fs.readFileSync(dspPath);
  const samples = decodeWavTo16kMono(fs.readFileSync(wavPath));
  const frames = Math.floor(samples.length / AUDIO_FRAME_SAMPLES);

  const lines: string[] = [];
  const provider = await DspAudioProvider.create(
    dspBuf.buffer.slice(dspBuf.byteOffset, dspBuf.byteOffset + dspBuf.byteLength),
    samplesPcm(samples),
    (frame) => {
      // Block RMS in the [-1,1) domain, like the native_sim harness.
      const base = frame.seq * AUDIO_FRAME_SAMPLES;
      let sumSq = 0;
      for (let i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        const s = (samples[base + i] ?? 0) / 32768;
        sumSq += s * s;
      }
      lines.push(
        formatDLine({ ...frame, rms: Math.sqrt(sumSq / AUDIO_FRAME_SAMPLES), gain: 0 }),
      );
    },
  );
  for (let f = 0; f < frames; f++) {
    provider.nextFrame(f);
  }
  lines.push(`#DONE frames=${frames} dropped=0`);

  const text = lines.join("\n") + "\n";
  if (outPath !== null) {
    fs.writeFileSync(outPath, text);
    process.stderr.write(`wrote ${frames} frames to ${outPath}\n`);
  } else {
    process.stdout.write(text);
  }
}
