# rgbx extension simulator (`fw/sim/`)

Runs `.llext`-style animation extensions **without hardware**: the same
single translation unit that builds to an ARM `.llext` is compiled to
WebAssembly and driven by a TypeScript harness that replicates the
firmware's `extension_host::tick()` semantics — nominal 33 ms ticks, 25 Hz
IMU / 31.25 Hz audio sample-and-hold, host-side COLOR-mode resolution,
brightness truncation, dead-pixel masking, and the fault/params-reset
rules. Audio features come from the **real** `audio_dsp.cpp` + CMSIS-DSP
compiled to WASM (parity-gated in CI against the native_sim replay
harness). See `PARITY.md` for exactly what is and isn't reproduced —
the ARM build and on-device verification stay mandatory.

Two frontends share one core:

- **`rgbx-sim` CLI** — headless, deterministic, JSON reports with ASCII
  frame renders: the agent/CI iteration loop.
- **Browser UI** (`rgbx-sim serve`) — live glasses rendering, param
  editors, buttons, live microphone through the real DSP, phone IMU.

## Hosted simulator

The browser UI is deployed at **https://rgb-sunglasses.autom8ed.com/sim/**
(built by `.github/workflows/pages.yml` on every relevant push to main).
It bundles the in-repo extensions and the real DSP, and — because it's
HTTPS — live microphone and phone motion work with zero setup.

**Load your own extension**: drag a `.wasm` file onto the page (or use
"Load .wasm…" in the top bar). Uploads are session-only by design — re-drop
a rebuilt file to iterate; nothing persists across visits. The file must be
a **wasm build** (`fw/sim/build-extensions.sh <name>`), not the ARM
`.llext` the device runs — dropping an `.llext` gets a targeted
explanation, since the simulator executes WebAssembly, never ARM code
(`PARITY.md`).

## Quick start

```bash
fw/sim/setup.sh                                   # once per checkout
fw/sim/rgbx-sim run cpptest --scenario silence     # build + simulate + report
fw/sim/rgbx-sim run hello --scenario metronome-120 --json   # full JSON report
fw/sim/rgbx-sim scenarios                         # list canned stimuli
fw/sim/rgbx-sim serve                             # browser UI
```

The wasm toolchain (pinned wasi-sdk) lives at `/opt/wasi-sdk-*` in the
devcontainer, or is downloaded once to `~/.cache/rgb-sunglasses/` by
`scripts/install-toolchain.sh` (macOS works too).

## The agent iteration loop

After editing `fw/extensions/foo/foo.c`:

```bash
fw/sim/rgbx-sim run foo --scenario metronome-120 --seconds 5 --json
```

Exit codes: **0** = ran + all scenario expectations passed (including
"expected fault" for crash/hang scenarios) · **1** = usage/build error
(compiler output on stderr) · **2** = unexpected fault · **3** =
expectation failed (black frames, invisible after brightness, golden
mismatch, missing expected fault).

The report (`report.json`, schema `rgbx-sim-report/1`; printed with
`--json`) is designed to answer "does it look right" from text alone:

- `frames.samples[].ascii` — 40×12 luma renders (dead cells blank);
- `frames.visibleAfterBrightness` — **false means your animation is
  invisible on the real panel** (the ×0.02 brightness trap: render near
  full-scale 255);
- `frames.regions` — per-lens/row luma + dominant channel,
  `noseCutoutWrites` (drawing into the hole);
- `frames.motionScore`, `uniqueFrameDigests` — is it animating at all;
- `audio.beatResponse.ratio/detected` — does it visibly react to beats;
- `result.fault` — kind (`trap` / `wall_backstop` / `cpu_budget` /
  `init_failed` / `bad_manifest`), detail, `paramsResetToDefaults`;
- `timing.tickWallMs` — sim wall time (device CPU budget is ~50-100×
  tighter, see PARITY.md);
- `printk` — the extension's log output (`%f` prints literally, like the
  device), each line tagged with the phase that produced it: `[init]` for
  `rgbx_init`, `[tick N]` for the tick that emitted it.

Useful flags: `--param Name=value` (repeatable; COLOR takes `0xMMRRGGBB`
with the mode byte in `MM`, FLOAT params take a decimal like `0.5`),
`--seed N`, `--ticks N`, `--png-every 30`
(PNG frames an agent can Read — ×10 upscale, bezel-painted cutout),
`--ansi 3` (live terminal preview), `--post-brightness` (render artifacts
as the wearer sees them), `--budget-ms/--backstop-ms`.

## Scenarios (`rgbx-scenario/1`)

Canned files in `fw/sim/scenarios/` (list with `rgbx-sim scenarios`);
pass a path for custom ones. Shape:

```jsonc
{
  "schema": "rgbx-scenario/1",
  "name": "my-test", "description": "...", "durationMs": 5000, "seed": 42,
  "audio": { "type": "metronome", "bpm": 120 },   // silence|metronome|sweep|noise|wav|features
  "imu":   { "type": "ramp", "fromAccel": [-10,0,3], "toAccel": [10,0,3] },
  "timeline": [
    { "atMs": 1000, "set": { "Speed": 200, "Color": "0x02000000" } },
    { "atMs": 2000, "press": "Up" }
  ],
  "expect": { "nonBlackBeforeMs": 500, "visibleAfterBrightness": true,
              "beatResponse": true }              // or "fault": {"kind": "trap"}
}
```

Audio types `metronome|sweep|noise|wav` synthesize/decode 16 kHz PCM and
feed it through the **real DSP** wasm; `features` replays a D-line dump
(device `sound dump` or replay-harness output) verbatim. Buttons are
one-tick edges; params are set by name; everything is seeded and
deterministic — same seed ⇒ bit-identical frames.

The browser UI replays scenarios too (Inputs tab → Scenario): bundled ones
are served via `/scenario-index.json` (assets included, dev and the `/sim/`
Pages build alike), and "Load scenario…" takes an uncommitted `.json` picked
together with the files it references (e.g. a fresh `capture_to_scenario.py`
output — refs resolve by basename). Playback drives audio + IMU + the
timeline through the same `core/scenarioProviders`/`core/scenarioTimeline`
code as the CLI; `expect{}` is a CLI/CI concern and is ignored in the
browser. Play/Restart tears down and rebuilds the host so every run starts
at frame 0 (there is deliberately no seek — same reason the CLI rejects
`--start-time-ms` for finite-stimulus scenarios).

## Goldens

`fw/sim/golden/<ext>/<scenario>.json` holds frame digests at fixed ticks
(text only, no binaries). `rgbx-sim compare` re-runs and diffs (exact —
the sim is deterministic, and wasm float semantics are platform-
independent); on mismatch it prints expected-vs-actual ASCII side by
side. After an intentional visual change: `rgbx-sim compare --update`
and commit.

## Audio-DSP parity

```bash
python3 fw/tools/beat_lab/replay.py --wav clip.wav --buckets --out host.txt
fw/sim/rgbx-sim dsp-replay --wav clip.wav --out sim.txt
python3 fw/tools/beat_lab/compare_sim.py host.txt sim.txt
```

Gates: per element ULP ≤ 64 **or** scale-relative ≤ 2e-5; beat masks ≤ 1%
frame mismatch. Measured on a click track (byte-identical PCM input): max
3.7e-7, 0 beat diffs. CI runs this in `build.yaml` (`dsp-parity` job).

## Browser UI / phone

```bash
fw/sim/rgbx-sim serve        # http://localhost:5173
```

- Live mic: uses `getUserMedia` with echo-cancellation/AGC/noise-
  suppression off, resampled to 16 kHz, 512-sample blocks through the
  real DSP. Secure context required (localhost qualifies).
- Phone (real IMU + mic): `adb reverse tcp:5173 tcp:5173`, then open
  `http://localhost:5173` in the phone's Chrome — localhost is a secure
  context, no certs. **Hold the `app` hw-lock first** if using the shared
  test phone (see root CLAUDE.md). For a personal phone on the LAN:
  `RGBX_SIM_SSL=1 npm run serve` and accept the self-signed warning.
- iOS DeviceMotion needs the on-page permission button (user gesture).

## Layout

```
build-extensions.sh   every fw/extensions/*/ TU -> out/wasm/<name>.wasm (+ audio_dsp.wasm)
shim/                 EXPORT_SYMBOL/printk/log shims, ABI offset asserts, DSP wrapper
core/                 platform-agnostic harness (abi, manifest, colorMode, display,
                      host, providers, pcm/imu generators, scenario types, sandbox)
node/                 CLI, worker adapter, report/PNG/ANSI/WAV/D-line, goldens
browser/              vite UI (canvas, params, sensors)
scenarios/ golden/    canned stimuli, digest snapshots
test/                 node:test suites (npm test; integration tests need a prior build)
```

CI: `.github/workflows/sim-ci.yml` (build, tests, scenario smoke, fault
paths, golden compare — plain Node runner) + the `dsp-parity` job in
`build.yaml` (devcontainer, west).
