#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Stuart Alldritt
//
// Post-link admission gate for a memoryless RGBX v2 module.
//
// Two independent passes, both of which must succeed:
//
//   1. A structural pass that re-derives the firmware's admission decision
//      from the module bytes. Every limit it applies comes from the SDK
//      manifest's pinned profile (rgbx-v2-policy.mjs), never from a constant
//      written down here, so this gate and the device cannot disagree about
//      what is admissible.
//   2. A tick oracle that actually runs rgbx_init and one rgbx_tick against a
//      host enforcing the same per-tick call budgets the firmware enforces,
//      proving the module paints a complete frame rather than only looking
//      like it could.
//
// Both passes live in rgbx-v2-host.mjs, which the simulator runs too: the
// gate that decides whether a package may ship and the simulator that shows
// you what it will look like are the same admission decision and the same
// host, not two implementations that agree today.
//
// The oracle executes an untrusted guest, so it never runs on this thread: it
// runs in a worker with a wall deadline and V8 resource limits, and anything
// that outlives or outgrows those bounds is a rejection, not a hang.

import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { isMainThread, parentPort, workerData, Worker } from "node:worker_threads";

import { admitModule, allCapabilities, RgbxV2Guest } from "./rgbx-v2-host.mjs";
import { loadSdkManifest, modulePolicy } from "./rgbx-v2-policy.mjs";

// Wall budget for one complete oracle run (compile, rgbx_init, one rgbx_tick).
// A conforming module finishes in single-digit milliseconds; this is sized so
// a loaded CI runner cannot produce a false rejection, while a guest that does
// not terminate is still bounded.
const ORACLE_DEADLINE_MS = 1000;

// V8 caps for the oracle worker. A memoryless module has no linear memory to
// grow, so these bound the interpreter and the host-side accounting rather
// than guest data, and they turn a pathological module into an out-of-memory
// rejection instead of host memory pressure.
const ORACLE_RESOURCE_LIMITS = {
  maxOldGenerationSizeMb: 64,
  maxYoungGenerationSizeMb: 16,
  codeRangeSizeMb: 32,
  stackSizeMb: 4,
};

function fail(message) {
  throw new Error(message);
}

// The oracle grants every capability the profile defines. A module's actual
// permissions live in the package manifest, which does not exist yet at
// post-link time, so gating here would reject a guest for reading an input
// its package will legitimately be granted. The device applies the package's
// real grant; this pass is only asking whether the guest paints a frame.
function executeOracle(bytes, policy) {
  const guest = new RgbxV2Guest(bytes, policy, { capabilities: allCapabilities(policy) });

  // Probe several time and parameter combinations, not one fixed tick. A guest
  // that paints a complete frame only when dt equals a magic value, or only for
  // one parameter set, would slip past a single probe; each combination here
  // must independently paint every pixel exactly once.
  const paramSets = [
    [50, 0x00ff40ff, 0, 0],
    [0, 0x00000000, 1, 0x00ffffff],
    [0xffffffff, 0x00123456, 0, 1],
    [7, 0x00abcdef, 1, 0x00808080],
  ];
  const deltas = [0, 1, 17, 1000, 0xffffffff];
  for (let probe = 0; probe < deltas.length; ++probe) {
    const values = paramSets[probe % paramSets.length];
    guest.inputs.params.fill(0);
    for (let slot = 0; slot < values.length; ++slot) guest.inputs.params[slot] = values[slot];
    const result = guest.tick(deltas[probe]);
    if (!result.ok) fail(`${result.reason} at dt=${deltas[probe]}`);
  }
}

function runOracleWithDeadline(bytes, manifest) {
  return new Promise((resolvePromise, rejectPromise) => {
    let settled = false;
    const worker = new Worker(new URL(import.meta.url), {
      // The worker receives the manifest the main thread already validated and
      // expands it itself, rather than re-reading it from disk.
      workerData: { bytes, manifest },
      resourceLimits: ORACLE_RESOURCE_LIMITS,
      // The gate gives the guest no filesystem or network reach, but an empty
      // environment keeps a future host callback from becoming one.
      env: {},
    });
    const settle = (error) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      void worker.terminate();
      if (error) rejectPromise(error);
      else resolvePromise();
    };
    const timer = setTimeout(() => {
      settle(new Error(`RGBX v2 guest oracle exceeded ${ORACLE_DEADLINE_MS} ms`));
    }, ORACLE_DEADLINE_MS);
    worker.once("message", (message) => {
      settle(message.ok ? undefined : new Error(message.error));
    });
    worker.once("error", (error) => settle(error));
    // A worker that dies without reporting, whether out of memory against
    // the resource limits above or killed, must fail the gate, never pass it.
    worker.once("exit", (code) => {
      settle(new Error(`RGBX v2 guest oracle worker exited with code ${code}`));
    });
  });
}

async function main() {
  const [moduleArg] = process.argv.slice(2);
  if (!moduleArg) {
    console.error("usage: check-rgbx-v2.mjs <module.wasm>");
    process.exit(2);
  }
  const bytes = readFileSync(resolve(moduleArg));
  const manifest = loadSdkManifest();
  const admitted = admitModule(bytes, modulePolicy(manifest));
  await runOracleWithDeadline(bytes, manifest);
  console.log(`RGBX v2 gate passed: ${bytes.length} bytes, ${admitted.importCount} imports`);
}

if (isMainThread) {
  await main();
} else {
  try {
    executeOracle(new Uint8Array(workerData.bytes), modulePolicy(workerData.manifest));
    parentPort.postMessage({ ok: true });
  } catch (error) {
    parentPort.postMessage({ ok: false, error: error.message });
  }
}
