/**
 * Tests for the real-DSP audio path (audio_dsp.wasm). Skipped if the
 * module hasn't been built — run fw/sim/build-extensions.sh first.
 */

import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { DspAudioProvider } from "../core/audio";
import { metronomePcm, noisePcm, silencePcm } from "../core/pcmGen";

const DSP_PATH = path.join(__dirname, "..", "..", "out", "wasm", "audio_dsp.wasm");
const skip = fs.existsSync(DSP_PATH)
  ? false
  : "audio_dsp.wasm not built — run fw/sim/build-extensions.sh";

function loadDsp(): ArrayBuffer {
  const buf = fs.readFileSync(DSP_PATH);
  return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
}

test("silence produces zero-ish energies and no beats", { skip }, async () => {
  const dsp = await DspAudioProvider.create(loadDsp(), silencePcm());
  for (let f = 0; f < 40; f++) {
    const feat = dsp.nextFrame(f);
    assert.equal(feat.beat.some((b) => b !== 0), false, `beat on silent frame ${f}`);
    assert.ok(feat.bandEnergy.every((e) => e < 1e-6));
  }
});

test("120 BPM metronome fires beats at ~2 Hz through the real detector", { skip }, async () => {
  const dsp = await DspAudioProvider.create(loadDsp(), metronomePcm({ bpm: 120 }));
  let beats = 0;
  const frames = Math.round(10_000 / 32); // 10 s of audio
  for (let f = 0; f < frames; f++) {
    const feat = dsp.nextFrame(f);
    if (feat.beat.some((b) => b !== 0)) {
      beats++;
    }
  }
  // 10 s @ 120 BPM = 20 clicks. The detector needs ~1 s of history to arm
  // its adaptive threshold and a click can land across two frames; accept a
  // generous window while still proving genuine periodic detection.
  assert.ok(beats >= 10 && beats <= 30, `${beats} beat frames for 20 clicks`);
});

test("pink noise excites the display buckets deterministically", { skip }, async () => {
  async function run(): Promise<number[]> {
    const dsp = await DspAudioProvider.create(loadDsp(), noisePcm({ color: "pink", seed: 99 }));
    let last: number[] = [];
    for (let f = 0; f < 20; f++) {
      last = Array.from(dsp.nextFrame(f).displayBucket);
    }
    return last;
  }
  const a = await run();
  const b = await run();
  assert.ok(a.some((v) => v > 0), "no bucket energy from noise");
  assert.deepEqual(a, b, "same seed must give identical DSP output");
});
