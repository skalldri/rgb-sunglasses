/**
 * rgbx-sim — headless CLI for the extension simulator. Invoked via the
 * fw/sim/rgbx-sim shim; see fw/sim/README.md for the full reference.
 *
 * Exit codes (the agent contract):
 *   0  ran to completion AND every scenario `expect` check passed
 *      (including "expected a fault and got it" for crash/hang scenarios)
 *   1  usage / toolchain / build error
 *   2  UNEXPECTED fault (trap, cpu budget, wall backstop, bad manifest)
 *   3  ran clean but an expectation failed (black frames, invisible after
 *      brightness, missing expected fault, golden mismatch)
 */

import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { parseScenario, Scenario } from "../core/scenario";
import { runScenario } from "./scenarioRun";
import { dspReplay } from "./dspReplay";
import { compareGoldens, updateGoldens } from "./golden";

const SIM_DIR = path.join(__dirname, "..", ".."); // dist/node -> fw/sim
const SCENARIO_DIR = path.join(SIM_DIR, "scenarios");
const WASM_DIR = path.join(SIM_DIR, "out", "wasm");

function usage(): never {
  process.stderr.write(`usage:
  rgbx-sim build <name|path> [...]      compile extension(s) to .wasm
  rgbx-sim run <name|path.wasm> [flags] build (if needed) + simulate + report
  rgbx-sim scenarios                    list canned scenarios
  rgbx-sim dsp-replay --wav <f> [--out <f>]   WAV through the wasm DSP -> D-lines
  rgbx-sim compare [--update]           check golden frame digests
  rgbx-sim serve                        start the browser UI (vite)

run flags:
  --scenario <name|file.json>  stimulus (default: silence)
  --seconds N | --ticks N      duration (default: 5 s ~ 152 ticks at dt=33)
  --start-time-ms N            warm-up: advance N ms of animation time BEFORE
                               recording starts, so a scenario can be replayed
                               from a non-zero animation time (default 0).
                               Recorded ticks are still numbered from 0.
  --seed N                     RNG seed (default: scenario's, else 0)
  --param Name=value           initial param override (repeatable; COLOR
                               accepts 0xMMRRGGBB with the mode byte)
  --json                       print the full report JSON to stdout
  --ascii N                    embed N ASCII frame samples (default 3)
  --png-every N                write frames/tick-*.png every N ticks
  --dump-frames N              write raw RGB dumps every N ticks
  --ansi N                     live ANSI terminal preview every N ticks
  --post-brightness            render artifacts through the device's x0.02
  --out DIR                    artifact dir (default fw/sim/out/runs/...)
  --budget-ms N / --backstop-ms N   fault thresholds (defaults 50 / 500)
  --no-build                   skip the implicit wasm build
`);
  process.exit(1);
}

function fail(msg: string): never {
  process.stderr.write(`rgbx-sim: ${msg}\n`);
  process.exit(1);
}

function buildExtensions(names: string[]): void {
  const script = path.join(SIM_DIR, "build-extensions.sh");
  const res = spawnSync("bash", [script, ...names], { stdio: "inherit" });
  if (res.status !== 0) {
    process.exit(1);
  }
}

function resolveWasm(nameOrPath: string, noBuild: boolean): string {
  if (nameOrPath.endsWith(".wasm")) {
    if (!fs.existsSync(nameOrPath)) {
      fail(`no such file: ${nameOrPath}`);
    }
    return path.resolve(nameOrPath);
  }
  const name = path.basename(nameOrPath);
  if (!noBuild) {
    buildExtensions([nameOrPath]);
  }
  const wasm = path.join(WASM_DIR, `${name}.wasm`);
  if (!fs.existsSync(wasm)) {
    fail(`${wasm} not found — build failed?`);
  }
  return wasm;
}

function loadScenario(ref: string): { scenario: Scenario; dir: string } {
  let file = ref;
  if (!ref.endsWith(".json")) {
    file = path.join(SCENARIO_DIR, `${ref}.json`);
    if (!fs.existsSync(file)) {
      const names = listScenarioNames().join(", ");
      fail(`no canned scenario "${ref}" (have: ${names})`);
    }
  } else if (!fs.existsSync(file)) {
    fail(`no such scenario file: ${file}`);
  }
  const scenario = parseScenario(JSON.parse(fs.readFileSync(file, "utf8")));
  return { scenario, dir: path.dirname(path.resolve(file)) };
}

function listScenarioNames(): string[] {
  if (!fs.existsSync(SCENARIO_DIR)) {
    return [];
  }
  return fs
    .readdirSync(SCENARIO_DIR)
    .filter((f) => f.endsWith(".json"))
    .map((f) => f.replace(/\.json$/, ""))
    .sort();
}

interface Flags {
  positional: string[];
  options: Map<string, string[]>;
  bools: Set<string>;
}

const BOOL_FLAGS = new Set(["json", "post-brightness", "no-build", "update"]);

function parseArgs(argv: string[]): Flags {
  const flags: Flags = { positional: [], options: new Map(), bools: new Set() };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (!arg.startsWith("--")) {
      flags.positional.push(arg);
      continue;
    }
    const key = arg.slice(2);
    if (BOOL_FLAGS.has(key)) {
      flags.bools.add(key);
      continue;
    }
    const value = argv[++i];
    if (value === undefined) {
      fail(`--${key} needs a value`);
    }
    const list = flags.options.get(key) ?? [];
    list.push(value);
    flags.options.set(key, list);
  }
  return flags;
}

function one(flags: Flags, key: string): string | undefined {
  const list = flags.options.get(key);
  if (list !== undefined && list.length > 1) {
    fail(`--${key} given more than once`);
  }
  return list?.[0];
}

function num(flags: Flags, key: string, dflt: number): number {
  const v = one(flags, key);
  if (v === undefined) {
    return dflt;
  }
  const n = Number(v);
  if (!Number.isFinite(n)) {
    fail(`--${key} must be a number, got "${v}"`);
  }
  return n;
}

async function cmdRun(argv: string[]): Promise<void> {
  const flags = parseArgs(argv);
  if (flags.positional.length !== 1) {
    usage();
  }

  const wasmPath = resolveWasm(flags.positional[0], flags.bools.has("no-build"));
  const { scenario, dir } = loadScenario(one(flags, "scenario") ?? "silence");

  const dtMs = 33;
  const seconds = num(flags, "seconds", scenario.durationMs / 1000);
  const ticks = Math.round(num(flags, "ticks", (seconds * 1000) / dtMs));
  // Warm-up ticks are not part of the recorded run: a defect whose cost grows with
  // a free-running accumulator is only reachable minutes in, and simulating that
  // whole warm-up is otherwise the only way to get there.
  const startTimeMs = num(flags, "start-time-ms", 0);
  if (startTimeMs < 0) {
    fail(`--start-time-ms must be >= 0, got ${startTimeMs}`);
  }
  // Upper bound: every warm-up tick is an awaited worker round-trip and nothing is
  // printed until the warm-up finishes, so a fat-fingered extra zero (or ms/us
  // confusion) is indistinguishable from the hung extension --backstop-ms exists to
  // surface. An hour of sim time is far past any plausible accumulator defect.
  const kMaxStartTimeMs = 60 * 60 * 1000;
  if (startTimeMs > kMaxStartTimeMs) {
    fail(
      `--start-time-ms must be <= ${kMaxStartTimeMs} (1 h of sim time), got ${startTimeMs}. ` +
        `That is ${Math.round(startTimeMs / dtMs).toLocaleString()} warm-up ticks, which would ` +
        `run for a long time printing nothing — check for an extra digit or us/ms confusion.`,
    );
  }
  // Warm-up consumes stimulus, it does not replay it: pumpTimeline() and host.tick()
  // advance the PROVIDERS too, and several of them are finite. Warming past their end
  // leaves the recorded window with none of the stimulus the scenario exists to apply
  // — a wav goes silent (zero-fill), a ramp/keyframe IMU freezes at its final sample,
  // a sweep holds its end tone, and every timeline press/set has already fired. The
  // extension is then scored against a blank input and fails an expectation it should
  // have passed. Refusing is deliberate: a false failure costs more than the feature
  // saves, and silently looping a finite source would change what the scenario means.
  const finiteSources: string[] = [];
  if (scenario.timeline !== undefined && scenario.timeline.length > 0) {
    finiteSources.push(
      `a timeline (${scenario.timeline.length} event(s) — all fire during the warm-up and are never observed)`,
    );
  }
  if (
    scenario.audio !== undefined &&
    (scenario.audio.type === "wav" ||
      scenario.audio.type === "features" ||
      scenario.audio.type === "sweep")
  ) {
    finiteSources.push(`a finite audio source (type "${scenario.audio.type}")`);
  }
  if (scenario.imu !== undefined && (scenario.imu.type === "ramp" || scenario.imu.type === "keyframes")) {
    finiteSources.push(`a finite IMU source (type "${scenario.imu.type}")`);
  }
  if (startTimeMs > 0 && finiteSources.length > 0) {
    fail(
      `--start-time-ms is not supported for scenario "${scenario.name}": it has ` +
        `${finiteSources.join(" and ")}. Warm-up would consume the stimulus before ` +
        `recording starts. Use it with a steady-state scenario (silence / metronome / ` +
        `noise audio, static / sine IMU, no timeline).`,
    );
  }
  const warmupTicks = Math.round(startTimeMs / dtMs);
  const seed = num(flags, "seed", scenario.seed ?? 0);

  const paramOverrides: Record<string, string> = {};
  for (const kv of flags.options.get("param") ?? []) {
    const eq = kv.indexOf("=");
    if (eq <= 0) {
      fail(`--param expects Name=value, got "${kv}"`);
    }
    paramOverrides[kv.slice(0, eq)] = kv.slice(eq + 1);
  }

  const extName = path.basename(wasmPath, ".wasm");
  const outDir =
    one(flags, "out") ?? path.join(SIM_DIR, "out", "runs", extName, scenario.name);
  fs.mkdirSync(outDir, { recursive: true });

  const { report, exitCode } = await runScenario({
    wasmPath,
    dspWasmPath: path.join(WASM_DIR, "audio_dsp.wasm"),
    scenario,
    scenarioDir: dir,
    seed,
    ticks,
    warmupTicks,
    dtMs,
    budgetMs: num(flags, "budget-ms", 50),
    backstopMs: num(flags, "backstop-ms", 500),
    paramOverrides,
    outDir,
    asciiSamples: num(flags, "ascii", 3),
    pngEvery: num(flags, "png-every", 0),
    dumpFrames: num(flags, "dump-frames", 0),
    ansiEvery: num(flags, "ansi", 0),
    postBrightness: flags.bools.has("post-brightness"),
  });

  const reportPath = path.join(outDir, "report.json");
  (report.artifacts as Record<string, string | null>).report = reportPath;
  fs.writeFileSync(reportPath, JSON.stringify(report, null, 2));

  if (flags.bools.has("json")) {
    process.stdout.write(JSON.stringify(report, null, 2) + "\n");
  } else {
    printSummary(report, reportPath);
  }
  process.exit(exitCode);
}

function printSummary(report: Record<string, unknown>, reportPath: string): void {
  const r = report as {
    extension: { name: string };
    run: { scenario: string; ticks: number; simulatedMs: number; hostWallMs: number };
    result: { status: string; fault: { kind: string; detail: string } | null };
    frames: {
      nonBlackTicks: number;
      visibleAfterBrightness: boolean;
      motionScore: number;
      samples: { tick: number; ascii: string }[];
    };
    audio: { beatResponse: { detected: boolean } };
    checks: { name: string; pass: boolean; detail?: string }[];
  };
  const lines: string[] = [];
  lines.push(
    `${r.extension.name} × ${r.run.scenario}: ${r.run.ticks} ticks (${(r.run.simulatedMs / 1000).toFixed(1)} s sim) in ${r.run.hostWallMs} ms`,
  );
  if (r.result.fault !== null) {
    lines.push(`FAULT [${r.result.fault.kind}] ${r.result.fault.detail}`);
  }
  lines.push(
    `frames: ${r.frames.nonBlackTicks} non-black, visibleAfterBrightness=${r.frames.visibleAfterBrightness}, motion=${r.frames.motionScore}`,
  );
  for (const c of r.checks) {
    lines.push(`  ${c.pass ? "PASS" : "FAIL"} ${c.name}${c.detail !== undefined ? ` (${c.detail})` : ""}`);
  }
  const sample = r.frames.samples[r.frames.samples.length - 1];
  if (sample !== undefined) {
    lines.push(`last sampled frame (tick ${sample.tick}):`);
    lines.push(sample.ascii.split("\n").map((row) => `  |${row}|`).join("\n"));
  }
  lines.push(`report: ${reportPath}`);
  process.stdout.write(lines.join("\n") + "\n");
}

function cmdScenarios(): void {
  for (const name of listScenarioNames()) {
    const s = parseScenario(
      JSON.parse(fs.readFileSync(path.join(SCENARIO_DIR, `${name}.json`), "utf8")),
    );
    process.stdout.write(`${name.padEnd(16)} ${s.description}\n`);
  }
}

async function main(): Promise<void> {
  const [cmd, ...rest] = process.argv.slice(2);
  switch (cmd) {
    case "build":
      buildExtensions(rest);
      break;
    case "run":
      await cmdRun(rest);
      break;
    case "scenarios":
      cmdScenarios();
      break;
    case "dsp-replay":
      await dspReplay(parseArgs(rest), fail, WASM_DIR);
      break;
    case "compare": {
      const flags = parseArgs(rest);
      const goldenDir = one(flags, "golden") ?? path.join(SIM_DIR, "golden");
      const code = flags.bools.has("update")
        ? await updateGoldens(goldenDir)
        : await compareGoldens(goldenDir);
      process.exit(code);
      break;
    }
    case "serve": {
      // No `browser` positional: Vite resolves its config file relative to a
      // positional root and would silently skip fw/sim/vite.config.ts (which
      // is what sets root/publicDir/the wasm index). See browser/smoke.md.
      const res = spawnSync("npx", ["vite", "--host"], {
        stdio: "inherit",
        cwd: SIM_DIR,
      });
      process.exit(res.status ?? 1);
      break;
    }
    default:
      usage();
  }
}

void main().catch((err: Error) => {
  fail(err.message);
});
