---
name: add-extension
description: Create or modify a sandboxed .llext animation extension (rgbx ABI) and build it to a .llext artifact — no hardware required for build; installing/running needs the board.
---

**Read `fw/extensions/README.md` first — it is the complete, accurate developer doc**
(sandbox model, ABI, parameters, inputs, building, installing, debug shell). The ABI
contract itself is `fw/include/rgbx/rgbx_api.h` (flat C, 5 exported symbols) with the
C++ wrapper in `fw/include/rgbx/rgbx_animation.h`. This skill only routes you and adds
the failure guardrails that aren't obvious until they bite.

## 1. Create: copy a template

New extension = a new directory `fw/extensions/<name>/` containing a **single** `.c`
or `.cpp` file (if a directory somehow has more than one source, build.sh compiles
the first `.cpp`, falling back to the first `.c` only if no `.cpp` exists). Copy one of:

- **`fw/extensions/hello/hello.c`** — raw C against `rgbx_api.h`. Kitchen-sink: every
  param type (UINT32/COLOR/BOOL/STRING), every input source (IMU/audio/buttons), and
  each of the 5 symbols (`rgbx_manifest`, `rgbx_inputs`, `rgbx_framebuffer`,
  `rgbx_init`, `rgbx_tick`) individually `EXPORT_SYMBOL`'d at the bottom of the file.
- **`fw/extensions/plasma/plasma.cpp`** — C++ via `rgbx_animation.h`: subclass
  `rgbx::Animation`, instantiate with the `RGBX_ANIMATION(Class, "Name", W, H, ...)`
  macro (which emits + exports the same 5 C symbols). The class must be **trivially
  destructible** (static_assert in the macro); no heap, no exceptions, no RTTI, no
  libm — nothing links a math library, so `sinf()` etc. fail symbol resolution at
  load time on the device. See plasma's integer-math `wave8()` for the workaround.

Framebuffer dims: 40×12 on proto0 (the host rejects a manifest whose dims don't match
the display).

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

- Compile success from step 2 — this is the whole off-device validation surface for
  your extension's code.
- Optionally `twister -T fw/tests/extensions/manifest -p native_sim` (suite
  `extensions.manifest`) — but this validates the **host's manifest validator**, not
  your extension. Nothing executes `rgbx_tick` off-device. **Never claim an extension
  was "tested in the simulator"** — report "compiles; on-device verification pending".

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

## API docs

```bash
mkdir -p fw/build && (cd fw/extensions && doxygen Doxyfile)   # output: fw/build/doxygen/html
```

(The Doxyfile's relative paths resolve against the CWD, so it must be run from
`fw/extensions/`, and `fw/build` must exist.)
