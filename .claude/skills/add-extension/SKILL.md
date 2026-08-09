---
name: add-extension
description: Create or modify a sandboxed .llext animation extension (rgbx ABI) and build it to a .llext artifact — no hardware required for build; installing/running needs the board.
---

**Read `fw/extensions/README.md` first — it is the complete, accurate developer doc**
(sandbox model, ABI, parameters, inputs, building, installing, debug shell). The ABI
contract itself is `fw/include/rgbx/rgbx_api.h` (flat C, 5 exported symbols) with the
C++ wrapper in `fw/include/rgbx/rgbx_animation.h`. This skill only routes you and adds
the failure guardrails that aren't obvious until they bite.

**This skill covers IN-REPO extensions** (`fw/extensions/<name>/`, built via the EDK
path below). A **standalone/community extension** — developed outside this repo — is a
different flow: route via the root `CLAUDE.md` task-routing table (its
standalone-extension row points at the design doc and the root `extensions/README.md`).
In that flow the SDK's build gates automatically enforce only the *mechanical*
guardrails — `ld -r` region layout, undefined symbols vs the device's export table,
llext-heap fit, the wasm zero-import/export contract. **§3's behavioral guardrails
(the string-param indexing trap, near-255 rendering, globals reset per activation,
async `rgbx_init` failure surfacing) apply unchanged and are NOT gated** — an
extension violating them builds green everywhere and misbehaves on-device. SDK code
itself lives in `fw/sdk/` (CI: `sdk-ci.yml`).

## 1. Create: copy a template

New extension = a new directory `fw/extensions/<name>/` containing a **single** `.c`
or `.cpp` file (if a directory somehow has more than one source, build.sh compiles
the first `.cpp`, falling back to the first `.c` only if no `.cpp` exists). Copy one of:

- **`fw/extensions/hello/hello.c`** — raw C against `rgbx_api.h`. Kitchen-sink: every
  param type (UINT32/COLOR/BOOL/STRING), every input source (IMU/audio/buttons), and
  each of the 5 symbols (`rgbx_manifest`, `rgbx_inputs`, `rgbx_framebuffer`,
  `rgbx_init`, `rgbx_tick`) individually `EXPORT_SYMBOL`'d at the bottom of the file.
- **`fw/extensions/cpptest/cpptest.cpp`** — C++ via `rgbx_animation.h`: subclass
  `rgbx::Animation`, instantiate with the `RGBX_ANIMATION(Class, "Name", W, H, ...)`
  macro (which emits + exports the same 5 C symbols). The class must be **trivially
  destructible** (static_assert in the macro); no heap, no exceptions, no RTTI.
  Callable firmware symbols are exactly `fw/sdk/arm/allowed-symbols.txt`: string/mem
  + printk/vprintk + the curated single-precision libm set (`sinf`, `cosf`, `atan2f`,
  `sqrtf`, …), 64-bit division helpers, and `memmove` (issue #295,
  `fw/src/extensions/extension_exports.c`). Anything else — notably ALL
  double-precision math (`sin`, `pow`; fpv5-sp soft-floats doubles) — fails symbol
  resolution at load on the device. cpptest's integer `wave8()` predates the libm
  exports and remains a fine low-cost pattern.

Framebuffer dims: 40×12 on proto0 (the host rejects a manifest whose dims don't match
the display).

Both templates stamp `RGBX_ABI_VERSION` (currently 1, as of 2026-07 — `#define` in
`fw/include/rgbx/rgbx_api.h`) into the manifest; the host rejects any `.llext` whose
`abi_version` doesn't match (`BadAbiVersion` in `fw/src/extensions/extension_manifest.cpp`),
so every extension must be rebuilt whenever the ABI version bumps.

## 2. Build (no hardware, no lock, fast)

```bash
fw/extensions/build.sh              # default build dir: fw/build
fw/extensions/build.sh <build-dir>  # explicit
```

Requires an **existing configured proto0 build** — it regenerates the LLEXT EDK via
`west build --build-dir fw/build --domain fw -t llext-edk`. On a fresh worktree run
`/build-proto0` once first; after that build.sh alone is seconds-fast. Success looks
like `built .../fw/build/extensions/<name>.llext (NNNN bytes)` per extension; failure
modes: `error: Zephyr SDK toolchain not found` (bad container), a west/cmake error
(build dir not configured — run `/build-proto0`), or plain compiler errors.

The loaded extension must fit the LLEXT heap — `CONFIG_LLEXT_HEAP_SIZE=24` (kilobytes)
in `fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf` (as of 2026-07), sized for the
largest *single* extension since only the active one is llext-resident. Sanity-check
build.sh's reported `.llext` byte size against it (a rough proxy: the loader copies
sections into the heap, so on-heap size ≈ file size, not exactly equal).

## 2b. Simulate (no hardware, no lock — the iteration loop)

The WASM simulator (`fw/sim/`, full docs `fw/sim/README.md`) runs your extension's
actual code against the firmware's tick semantics and the REAL audio DSP:

```bash
fw/sim/rgbx-sim run <name> --scenario silence --json        # baseline render
fw/sim/rgbx-sim run <name> --scenario metronome-120 --json  # audio-reactive?
fw/sim/rgbx-sim run <name> --scenario head-tilt --json      # IMU-reactive?
```

(First run auto-installs npm deps + the wasi-sdk toolchain; `fw/sim/setup.sh` does it
eagerly. Needs no proto0 build, no board, no locks.)

Read the JSON report, not the pixels: `frames.samples[].ascii` (40×12 luma render),
`frames.visibleAfterBrightness` (**false = invisible on the real panel** — the 0.02
brightness trap from §3), `frames.regions` + `motionScore` (is it animating where you
think), `audio.beatResponse.detected`, `result.fault` (kind + `paramsResetToDefaults`),
`printk`. Exit codes: 0 pass, 1 build error, 2 unexpected fault, 3 expectation failed.
`rgbx-sim scenarios` lists all stimuli; `--param Name=value` overrides params;
`--png-every 30` writes viewable PNG frames; custom scenario JSON = timelines of param
writes + button presses + audio/IMU generators (schema in `fw/sim/README.md`).

Iterate here until the sim is clean, THEN do the ARM build (§2) — both must pass.

## 3. Guardrails (each one is a real, observed failure)

- **Never bypass build.sh for C++.** Its `ld -r` partial link is mandatory: C++
  COMDAT group sections interleave with `.data`/`.bss` in file offsets and the llext
  loader rejects the file on-device with `Region 0 ELF file range ... overlaps with 1`.
- **The `llext-edk` cmake target does not track rgbx header changes.** build.sh
  `rm -f`s the stale `llext-edk.tar.xz` before regenerating; any hand-rolled flow
  must do the same or you compile against stale headers.
- **String params — the classic trap.** The i-th STRING-typed param *counting only
  string params, in manifest declaration order* lives in
  `rgbx_inputs.param_strings[i]`; `params[i]` is **unspecified** for string params
  (see the `struct rgbx_inputs` doc comment in `rgbx_api.h`). The C++ wrapper's
  `paramString(index)` does the mapping for you. Limits: `RGBX_MAX_PARAMS` (16)
  params total, `RGBX_MAX_STRING_PARAMS` (4) strings, each ≤ `RGBX_PARAM_STRING_MAX-1`
  (31) bytes.
- **Render near full-scale (255) channel values.** The pattern controller multiplies
  every pixel by the global brightness factor (default 0.02); an animation drawing at
  32/255 is invisible and looks exactly like a crash.
- **Globals reset on every activation** (load-on-activate: the ELF is reloaded and
  `rgbx_init()` reruns fresh each time). No cross-activation state — persist nothing.
- **`rgbx_init` failure surfaces asynchronously** — via a fault + Is Active
  notification, never as an activation return value (activation only queues the load;
  the pattern-controller thread does the real bring-up lazily on the next frame).

## 4. What you can honestly validate without hardware

- **Simulator runs (§2b)**: your `rgbx_tick` logic, rendering, param/string/input
  handling, brightness visibility, beat/IMU reactivity, and fault behavior — the sim
  executes your real code with the firmware's real tick semantics and real audio DSP.
- **ARM compile (§2)**: linker pressure the sim can't reproduce — the sim links
  libc/libm statically, so **any call outside the device's exported surface (e.g.
  double-precision `sin()`, `sinhf()`) works in the sim and still fails llext load
  on the device**. Also the `ld -r` section layout and the 24 KB heap fit. Both §2
  and §2b must pass; neither substitutes for the other. Full divergence list:
  `fw/sim/PARITY.md` (notably: CPU budget is only wall-clock-approximated — device
  timing still needs the board).
- Optionally `twister -T fw/tests/extensions/manifest -p native_sim` (suite
  `extensions.manifest`) — but this validates the **host's manifest validator**, not
  your extension.
- **The sim cannot show you time-dependent transcendental cost.** Its libm reaches the
  expensive argument-reduction path ~2e6x further out than the device's does, and per
  `fw/sim/PARITY.md` sim CPU cost is only wall-clock-approximated on a host 50-100x
  faster than the M33 — so a long, clean sim run is *not* evidence that a phase
  accumulator is bounded. That check belongs on hardware (§5).
- **Honest claim wording**: "compiles for ARM; passes sim scenarios X/Y/Z; on-device
  load/render verification pending". The sim does NOT prove llext loading, MPU
  behavior, or timing budgets.

## 5. Install & run (board lock required)

Do not duplicate provisioning by hand — `/provision-device` builds and pushes every
extension (plus GLIM assets) and verifies via `ext list`. For a single-file manual
push, follow "Installing on the device" in `fw/extensions/README.md` (mount the USB
mass-storage disk per `fw/CLAUDE.md` "USB Flash Disk", `cp` the `.llext` into the
`ext/` subdirectory, `sync`, `umount`, then reboot the board — the firmware only
re-discovers extensions on boot). If you changed **host-side** extension code
(`fw/src/extensions/`), that's a firmware change: use `/flash-and-verify`.

Debug over the Zephyr shell (`mcp__serial__*`, see `fw/CLAUDE.md`):
`ext list` / `ext select <slot>` / `ext param <slot> <idx> [<value>]` / `ext stats`.
A crashed/hung extension shows `[FAULTED]`, its BLE activation is rejected, and
**only `ext select <slot>` clears the fault** — that's deliberate recovery design,
not a bug.

**Soak before you sign off — a ten-second `ext stats` proves nothing about cost.**
If your extension uses `sinf`/`cosf`/`tanf` with a phase accumulator, its per-tick
cost can grow with elapsed time (issue #304: 3.4 ms -> 25 ms over five minutes), and
the statistics are **reset on every activation** — the shell command, an app/BLE
switch, a shuffle rotation, or the boot restore — so a reading taken just after
selecting only ever shows the fast phase. While you hold the lock:

1. Leave it running **several minutes** on one uninterrupted activation, then read
   `ext stats` — don't switch animations in between, or you have zeroed it.
2. Watch the console for `Render overran the tick interval ...`. That is the direct
   symptom and it appears long before anything approaches the CPU budget, which sits
   ~4.5x above the render interval and will not catch this.
3. To compress the timeline, raise the speed parameter — cost that depends on elapsed
   animation-time arrives proportionally sooner.

Background and the correct wrap idiom: "Bound your phase accumulators" in
`fw/extensions/README.md`.

## API docs

```bash
mkdir -p fw/build && (cd fw/extensions && doxygen Doxyfile)   # output: fw/build/doxygen/html
```

(The Doxyfile's relative paths resolve against the CWD, so it must be run from
`fw/extensions/`, and `fw/build` must exist.)
