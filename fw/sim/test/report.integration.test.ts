/**
 * Report-level tests: the printk chain END TO END, from the sandbox through
 * SimHost, RunStats and buildReport to the `printk` array in report.json.
 *
 * host.integration.test.ts covers SimHost.initLog, but that is only the first
 * link. Everything after it — scenarioRun draining initLog, RunStats tagging
 * the lines, buildReport emitting them — had no test at all, so deleting the
 * `stats.recordInitLog(host.initLog)` call or gutting `pushLog()` left the
 * whole suite green while `rgbx-sim run` silently went back to `"printk": []`.
 * That is the exact regression this file exists to catch.
 */

import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { RunStats } from "../node/report";
import { runScenario } from "../node/scenarioRun";
import type { Scenario } from "../core/scenario";

const SIM_DIR = path.join(__dirname, "..", "..");
const WASM_DIR = path.join(SIM_DIR, "out", "wasm");
const helloPath = path.join(WASM_DIR, "hello.wasm");
const skip = !fs.existsSync(helloPath)
  ? "extension .wasm modules not built — run fw/sim/build-extensions.sh"
  : false;

const scenario: Scenario = {
  schema: "rgbx-scenario/1",
  name: "printk-chain",
  description: "in-test scenario: drives hello just long enough to drain init + first-tick printk",
  durationMs: 500,
  seed: 0,
  audio: { type: "silence" },
  imu: { type: "static" },
};

function runOpts(ticks: number, paramOverrides: Record<string, number | string> = {}) {
  return {
    wasmPath: helloPath,
    dspWasmPath: path.join(WASM_DIR, "audio_dsp.wasm"),
    scenario,
    scenarioDir: path.join(SIM_DIR, "scenarios"),
    seed: 0,
    ticks,
    warmupTicks: 0,
    dtMs: 11,
    budgetMs: 50,
    backstopMs: 500,
    paramOverrides,
    outDir: null,
    asciiSamples: 0,
    pngEvery: 0,
    dumpFrames: 0,
    ansiEvery: 0,
    postBrightness: false,
  };
}

test("report.printk carries init output, tagged and ordered", { skip }, async () => {
  const { report } = await runScenario(runOpts(3));
  const printk = report.printk as string[];

  // The bug this fixes: rgbx_init's line never reached the report at all.
  assert.equal(printk[0], "[init] hello: rgbx_init running inside the sandbox");

  // ...and it is tagged distinctly from tick output, which README.md now
  // documents. Init precedes tick 0, so ordering is part of the contract.
  assert.equal(printk[1], "[tick 0] hello: tick1 dt=11 speed=050 msg=ok");
  assert.equal(printk.length, 2, JSON.stringify(printk));
});

test("RunStats tags a faulted tick's log like any other tick", { skip: false }, () => {
  const stats = new RunStats(new Set<number>());
  stats.recordInitLog("init line\n");
  // record() never runs for a faulted tick — there is no framebuffer to
  // account — so this is the only path by which the log that explains a trap
  // reaches the report.
  stats.recordFaultLog(137, "about to deref null\n");
  assert.deepEqual(stats.logLines, [
    "[init] init line",
    "[tick 137] about to deref null",
  ]);
});

test("RunStats drops empty logs and splits multi-line output", { skip: false }, () => {
  const stats = new RunStats(new Set<number>());
  stats.recordInitLog("");
  assert.deepEqual(stats.logLines, []);
  stats.recordFaultLog(-2, "a\nb\n");
  // Negative tick = the fault happened during warm-up, before the recorded
  // window; the report numbers those negatively elsewhere too.
  assert.deepEqual(stats.logLines, ["[tick -2] a", "[tick -2] b"]);
});

/*
 * The faulted tick's printk, end to end.
 *
 * TickFault carries the log the worker drained on the way down — it captures
 * it precisely because traps often follow logs — but scenarioRun's fault
 * branch used to copy only `out.fault` and break, so the single line that
 * explains a trap never reached report.json. That drain now exists, and this
 * is what proves it is actually wired: the RunStats-level tests above would
 * pass just as happily with the scenarioRun call deleted.
 *
 * Getting a genuine logs-then-traps tick is the whole difficulty. It needs
 * hello's one-shot line to be emitted BEFORE its Crash trigger fires, which
 * is why that ordering in hello.c is deliberate and commented — with Crash
 * forced from t=0, tick 0 logs and then traps in the same call.
 */
test("report.printk keeps the log of the tick that trapped", { skip }, async () => {
  const { report, exitCode } = await runScenario(runOpts(5, { Crash: "true" }));
  const printk = report.printk as string[];
  const result = report.result as { fault: { kind: string; tick: number } | null };

  // The trap must land on tick 0 — the same tick that logged.
  assert.notEqual(result.fault, null, JSON.stringify(result));
  assert.equal(result.fault!.kind, "trap");
  assert.equal(result.fault!.tick, 0);
  assert.equal(exitCode, 2); // unexpected fault

  // The line the extension printed on its way down survives into the report.
  assert.deepEqual(printk, [
    "[init] hello: rgbx_init running inside the sandbox",
    "[tick 0] hello: tick1 dt=11 speed=050 msg=ok",
  ]);
});
