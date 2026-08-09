/**
 * Golden frame-digest snapshots (text-only — the repo checks in no
 * binaries). One JSON per (extension, scenario) pair under fw/sim/golden/:
 * digests at fixed ticks plus an ASCII render of the last sampled tick for
 * agent-debuggable diffs. Exact match is required — the sim is fully
 * deterministic (seeded RNG, nominal clock, wasm float semantics are
 * platform-independent).
 */

import * as fs from "node:fs";
import * as path from "node:path";
import { parseScenario, Scenario } from "../core/scenario";
import { runScenario } from "./scenarioRun";
import type { FrameSample } from "./report";

const SIM_DIR = path.join(__dirname, "..", ".."); // dist/node -> fw/sim
const SCENARIO_DIR = path.join(SIM_DIR, "scenarios");
const WASM_DIR = path.join(SIM_DIR, "out", "wasm");

/** The (extension, scenario) pairs goldens exist for. Both in-repo
 * examples, one static-ish scenario and one audio-reactive one. */
const GOLDEN_SPECS: { ext: string; scenario: string }[] = [
  { ext: "hello", scenario: "buttons-tour" },
  { ext: "hello", scenario: "metronome-120" },
  { ext: "cpptest", scenario: "silence" },
  { ext: "cpptest", scenario: "color-modes" },
];

interface GoldenFile {
  schema: "rgbx-sim-golden/1";
  extension: string;
  scenario: string;
  seed: number;
  ticks: number;
  samples: { tick: number; digest: string; ascii: string }[];
}

function goldenTicks(totalTicks: number): number[] {
  const ticks: number[] = [];
  for (let t = 10; t < totalTicks; t += 50) {
    ticks.push(t);
  }
  ticks.push(totalTicks - 1);
  return ticks;
}

async function runForGolden(spec: { ext: string; scenario: string }): Promise<GoldenFile> {
  const scenarioPath = path.join(SCENARIO_DIR, `${spec.scenario}.json`);
  const scenario: Scenario = parseScenario(JSON.parse(fs.readFileSync(scenarioPath, "utf8")));
  const dtMs = 11;
  const ticks = Math.round(scenario.durationMs / dtMs);
  const seed = scenario.seed ?? 0;
  const wasmPath = path.join(WASM_DIR, `${spec.ext}.wasm`);
  if (!fs.existsSync(wasmPath)) {
    throw new Error(`${wasmPath} not built — run fw/sim/build-extensions.sh`);
  }

  const { report } = await runScenario({
    wasmPath,
    dspWasmPath: path.join(WASM_DIR, "audio_dsp.wasm"),
    scenario,
    scenarioDir: SCENARIO_DIR,
    seed,
    ticks,
    // Goldens are always cold-start: a warm-up would change every recorded digest
    // and make the stored frames depend on a flag rather than on the extension.
    warmupTicks: 0,
    dtMs,
    budgetMs: 50,
    backstopMs: 500,
    paramOverrides: {},
    outDir: null,
    asciiSamples: 0,
    sampleTicksOverride: goldenTicks(ticks),
    pngEvery: 0,
    dumpFrames: 0,
    ansiEvery: 0,
    postBrightness: false,
  });

  const frames = report.frames as { samples: FrameSample[] };
  return {
    schema: "rgbx-sim-golden/1",
    extension: spec.ext,
    scenario: spec.scenario,
    seed,
    ticks,
    samples: frames.samples
      .sort((a, b) => a.tick - b.tick)
      .map((s, i, arr) => ({
        tick: s.tick,
        digest: s.digest,
        // ASCII only for the last sample — keeps the files small while
        // still giving mismatches a human/agent-readable anchor.
        ascii: i === arr.length - 1 ? s.ascii : "",
      })),
  };
}

function goldenPath(dir: string, spec: { ext: string; scenario: string }): string {
  return path.join(dir, spec.ext, `${spec.scenario}.json`);
}

export async function updateGoldens(dir: string): Promise<number> {
  for (const spec of GOLDEN_SPECS) {
    const golden = await runForGolden(spec);
    const file = goldenPath(dir, spec);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, JSON.stringify(golden, null, 2) + "\n");
    process.stdout.write(`updated ${file} (${golden.samples.length} samples)\n`);
  }
  return 0;
}

export async function compareGoldens(dir: string): Promise<number> {
  let failures = 0;
  for (const spec of GOLDEN_SPECS) {
    const file = goldenPath(dir, spec);
    if (!fs.existsSync(file)) {
      process.stderr.write(`MISSING ${file} — run \`rgbx-sim compare --update\`\n`);
      failures++;
      continue;
    }
    const golden = JSON.parse(fs.readFileSync(file, "utf8")) as GoldenFile;
    const actual = await runForGolden(spec);
    const actualByTick = new Map(actual.samples.map((s) => [s.tick, s]));
    let specFailed = false;
    for (const g of golden.samples) {
      const a = actualByTick.get(g.tick);
      if (a === undefined || a.digest !== g.digest) {
        specFailed = true;
        process.stderr.write(
          `MISMATCH ${spec.ext}×${spec.scenario} tick ${g.tick}: expected ${g.digest}, got ${a?.digest ?? "(no sample)"}\n`,
        );
        if (g.ascii !== "" && a !== undefined) {
          process.stderr.write(sideBySide(g.ascii, a.ascii) + "\n");
        }
      }
    }
    if (specFailed) {
      failures++;
    } else {
      process.stdout.write(`OK ${spec.ext}×${spec.scenario} (${golden.samples.length} samples)\n`);
    }
  }
  if (failures > 0) {
    process.stderr.write(
      `${failures} golden(s) mismatched. If the change is intentional, regenerate with \`rgbx-sim compare --update\` and commit.\n`,
    );
    return 3;
  }
  return 0;
}

function sideBySide(expected: string, actual: string): string {
  const e = expected.split("\n");
  const a = actual.split("\n");
  const lines = ["  expected".padEnd(44) + "actual"];
  for (let i = 0; i < Math.max(e.length, a.length); i++) {
    lines.push(`  |${(e[i] ?? "").padEnd(40)}|  |${(a[i] ?? "").padEnd(40)}|`);
  }
  return lines.join("\n");
}
