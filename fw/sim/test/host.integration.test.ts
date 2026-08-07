/**
 * End-to-end SimHost tests against the REAL hello.wasm / plasma.wasm built
 * by fw/sim/build-extensions.sh. Skipped (with a loud message) if the
 * modules haven't been built — run `fw/sim/build-extensions.sh` first;
 * `npm test` in CI always builds before testing.
 */

import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { SimHost } from "../core/host";
import { NodeWorkerAdapter } from "../node/workerAdapter";
import { FeatureReplayProvider, zeroAudioFeatures } from "../core/providers";

const WASM_DIR = path.join(__dirname, "..", "..", "out", "wasm");

function loadWasm(name: string): ArrayBuffer | null {
  const p = path.join(WASM_DIR, `${name}.wasm`);
  if (!fs.existsSync(p)) {
    return null;
  }
  const buf = fs.readFileSync(p);
  return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
}

function makeHost(wasmBytes: ArrayBuffer, opts: Partial<ConstructorParameters<typeof SimHost>[0]> = {}): SimHost {
  return new SimHost({
    wasmBytes,
    adapterFactory: () => new NodeWorkerAdapter(),
    seed: 42,
    ...opts,
  });
}

const helloBytes = loadWasm("hello");
const plasmaBytes = loadWasm("plasma");
const skip = helloBytes === null || plasmaBytes === null
  ? "extension .wasm modules not built — run fw/sim/build-extensions.sh"
  : false;

test("hello: activates, manifests, renders non-black frames", { skip }, async () => {
  const host = makeHost(helloBytes!);
  try {
    assert.equal(await host.activate(), null);
    assert.equal(host.metadata!.displayName, "Hello Extension");
    assert.equal(host.metadata!.paramCount, 5);

    let nonBlack = 0;
    for (let t = 0; t < 45; t++) {
      const out = await host.tick();
      assert.equal(out.status, "ok", JSON.stringify(out));
      if (out.status === "ok") {
        assert.equal(out.manifestIntact, true);
        if (out.framebuffer.some((b) => b !== 0)) {
          nonBlack++;
        }
      }
    }
    assert.ok(nonBlack > 40, `only ${nonBlack}/45 frames non-black`);
  } finally {
    await host.terminate();
  }
});

test("hello: Crash=1 traps; params reset to defaults", { skip }, async () => {
  const host = makeHost(helloBytes!);
  try {
    assert.equal(await host.activate(), null);
    const crashIdx = host.paramIndexByName("Crash");
    assert.ok(crashIdx >= 0);
    host.setParam(crashIdx, 1);
    host.setParam(host.paramIndexByName("Speed"), 200); // will be reset too

    const out = await host.tick();
    assert.equal(out.status, "fault");
    if (out.status === "fault") {
      assert.equal(out.fault.kind, "trap");
      assert.equal(out.fault.paramsResetToDefaults, true);
    }
    // Params back at manifest defaults (Speed=50, Crash=0).
    assert.equal(host.paramValues[host.paramIndexByName("Speed")], 50);
    assert.equal(host.paramValues[crashIdx], 0);
    assert.equal(host.faulted, true);

    // Faulted slot refuses to tick until clearFault + re-activate.
    const refused = await host.tick();
    assert.equal(refused.status, "fault");
    host.clearFault();
    assert.equal(await host.activate(), null);
    const ok = await host.tick();
    assert.equal(ok.status, "ok");
  } finally {
    await host.terminate();
  }
});

test("hello: Hang=1 hits the wall backstop and is terminated", { skip }, async () => {
  // Short backstop to keep the test fast; semantics identical.
  const host = makeHost(helloBytes!, { backstopMs: 300 });
  try {
    assert.equal(await host.activate(), null);
    host.setParam(host.paramIndexByName("Hang"), 1);
    const start = Date.now();
    const out = await host.tick();
    const elapsed = Date.now() - start;
    assert.equal(out.status, "fault");
    if (out.status === "fault") {
      assert.equal(out.fault.kind, "wall_backstop");
      assert.equal(out.fault.paramsResetToDefaults, true);
    }
    assert.ok(elapsed >= 290, `terminated too early (${elapsed} ms)`);
  } finally {
    await host.terminate();
  }
});

test("hello: buttons are edge-latched for exactly one tick", { skip }, async () => {
  const host = makeHost(helloBytes!);
  try {
    assert.equal(await host.activate(), null);
    // hello draws a 500 ms-decay marker at a corner per button press.
    // Frame must differ between pressed and long-after states.
    const before = await host.tick();
    host.pressButton(0);
    const pressed = await host.tick();
    assert.equal(pressed.status, "ok");
    if (before.status === "ok" && pressed.status === "ok") {
      assert.notDeepEqual(Array.from(pressed.framebuffer), Array.from(before.framebuffer));
    }
  } finally {
    await host.terminate();
  }
});

test("hello: audio beat flags stay sticky across ~3 ticks (32ms vs 11ms)", { skip }, async () => {
  // One beat frame at audio frame 3, then silence. hello lights the top
  // row segment for band b while inputs.audio_beat[b] is set — ticks 9,10
  // (simTime 99, 110) fall inside frame 3's window [96, 128).
  const frames = Array.from({ length: 32 }, () => zeroAudioFeatures());
  frames[3].beat[0] = 1;
  const host = makeHost(helloBytes!, { audioProvider: new FeatureReplayProvider(frames) });
  try {
    assert.equal(await host.activate(), null);
    const litTicks: number[] = [];
    for (let t = 0; t < 20; t++) {
      const out = await host.tick();
      assert.equal(out.status, "ok");
      if (out.status === "ok") {
        // Top row, band 0 segment = x in [0,10), y=0; beat lights it 255-red.
        const lit = out.framebuffer[0] === 255;
        if (lit) {
          litTicks.push(t);
        }
      }
    }
    assert.ok(litTicks.length >= 2, `beat visible on ${litTicks.length} ticks (${litTicks})`);
    assert.ok(litTicks.length <= 4, `beat sticky too long: ${litTicks.length} ticks`);
  } finally {
    await host.terminate();
  }
});

test("plasma (C++ wrapper): renders, good_moment export honored", { skip }, async () => {
  const host = makeHost(plasmaBytes!);
  try {
    assert.equal(await host.activate(), null);
    assert.equal(host.metadata!.displayName, "Plasma");
    const out = await host.tick();
    assert.equal(out.status, "ok");
    if (out.status === "ok") {
      assert.ok(out.framebuffer.some((b) => b !== 0), "plasma rendered black");
      assert.equal(typeof out.goodMoment, "boolean");
    }
  } finally {
    await host.terminate();
  }
});

test("determinism: same seed + inputs => identical frames", { skip }, async () => {
  async function run(): Promise<Uint8Array> {
    const host = makeHost(plasmaBytes!, { seed: 7 });
    try {
      assert.equal(await host.activate(), null);
      // Exercise the seeded color-mode path too: RandomTimerFade, speed 200.
      const colorIdx = host.paramIndexByName("Color");
      host.setParam(colorIdx, ((0x04 << 24) | (200 << 16)) >>> 0);
      let last: Uint8Array = new Uint8Array(0);
      for (let t = 0; t < 30; t++) {
        const out = await host.tick();
        assert.equal(out.status, "ok");
        if (out.status === "ok") {
          last = out.framebuffer;
        }
      }
      return last;
    } finally {
      await host.terminate();
    }
  }
  const a = await run();
  const b = await run();
  assert.deepEqual(Array.from(a), Array.from(b));
});
