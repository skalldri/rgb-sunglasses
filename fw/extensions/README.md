# Animation Extensions

Loadable animation extensions (`.llext` files) run **fully sandboxed**: the
firmware executes extension code exclusively on a dedicated user-mode thread
confined to a private MPU memory domain (GitHub issue #85). A buggy extension
can hang or crash *itself* — the firmware aborts the sandbox on a missed tick
deadline or MPU fault, keeps running, un-marks the animation's Is Active
characteristic (with a notification, so the app disables it), scrolls a
`FAULT: <name>` banner on the panel until you switch animations, and rejects
further BLE activation until the fault is deliberately cleared with
`ext select` on the shell.

Extensions are discovered at boot from `/NAND:/ext/*.llext` and appear as
**first-class animations**: they get their own BLE GATT service (Animation
Name + Is Active + one characteristic per declared parameter) that the
companion app renders exactly like a built-in animation, with zero app-side
changes.

**Load-on-activate:** discovery only validates each file and copies its
manifest out — the ELF is unloaded again immediately. Only the *active*
extension is resident in RAM; activation loads it (lazily, on the pattern
controller's next frame) and runs `rgbx_init()` fresh each time, so globals
reset on every activation. A bring-up failure is reported asynchronously via
the Is Active notification. Up to 16 extensions register per boot
(`extension_host::kMaxExtensions`), sorted by filename; each gets animation
id `0x40 + slot`.

## The ABI

`include/rgbx/rgbx_api.h` (flat C, ABI v1) is the whole contract. An extension
exports five symbols — a `rgbx_manifest` (name, framebuffer dims, parameter
table), a writable `rgbx_inputs` block the host fills before each tick, a
`rgbx_framebuffer` it renders into, and `rgbx_init`/`rgbx_tick` functions —
and never calls into the firmware. See `hello/hello.c` for a complete raw-C
extension exercising the full surface.

### Optional exports

Optional capabilities are negotiated by **symbol presence**, not by the ABI
version: the host looks each one up with a nullptr-tolerant `llext_find_sym()`
and applies a documented default when the symbol is absent, so extensions
built before an optional export existed keep loading and running unchanged.
A present-but-invalid optional export (outside extension memory) rejects the
extension, same as the required ones.

| Symbol | Default when absent | Meaning |
| ------ | ------------------- | ------- |
| `rgbx_good_moment` (`uint8_t`) | every frame is a good moment | Set during `rgbx_tick()`: nonzero means the frame just rendered ended at a natural switch boundary (end of a scroll/clip/cycle), so the firmware's shuffle mode (issue #121) can switch animations without visual jarring. |

Raw-C extensions define + `EXPORT_SYMBOL` it themselves (see `hello/hello.c`,
which signals on its scan-head wrap). C++ wrapper extensions get it for free:
`RGBX_ANIMATION()` always emits the symbol, driven by the `rgbx::Animation::goodMoment()`
virtual (default `true` — override it to signal real boundaries; `plasma.cpp`
doesn't and needs no changes).

### Parameters

Up to `RGBX_MAX_PARAMS` (16) parameters, each surfaced as a BLE
characteristic with the same presentation format the built-ins use:

| Type                | App control  | Value                                        |
| ------------------- | ------------ | -------------------------------------------- |
| `RGBX_PARAM_UINT32` | number field | `rgbx_inputs.params[i]`                      |
| `RGBX_PARAM_COLOR`  | color picker | `params[i]` as `0x00RRGGBB`                  |
| `RGBX_PARAM_BOOL`   | toggle       | `params[i]` as 0/1                           |
| `RGBX_PARAM_STRING` | text field   | `rgbx_inputs.param_strings[s]` (see below)   |

Declare them with `RGBX_PARAM(name, type, default)` /
`RGBX_PARAM_STR(name, "default")`. String values are capped at
`RGBX_PARAM_STRING_MAX-1` (31) bytes, at most `RGBX_MAX_STRING_PARAMS` (4)
per extension; the *i-th string-typed param in declaration order* reads from
`param_strings[i]` (the C++ wrapper's `paramString(index)` does this mapping
for you).

### Inputs

The host snapshots every source into `rgbx_inputs` before each tick (absent
sources read as zeros):

- **IMU** — `accel[3]` (m/s²), `gyro[3]` (rad/s).
- **Audio** — `audio_band_energy[4]` + `audio_beat[4]` (beat detector) and
  `audio_display_bucket[20]` (~0..1 spectrum buckets for bar graphs).
- **Buttons** — `buttons_pressed` bitmask, pressed-since-last-tick; proto0:
  bit 0=Up, 1=Left, 2=Right, 3=Down, 4=Wake.

### C++ wrapper

C++ authors can use `include/rgbx/rgbx_animation.h` instead: subclass
`rgbx::Animation`, then instantiate with `RGBX_ANIMATION(Class, "Name", W, H,
RGBX_PARAM(...))`. It adds typed accessors (`paramU32/paramColor/paramBool/
paramString`, `bandEnergy/isBeat/displayBucket`, `buttonWasPressed`, accel/
gyro getters). Nothing C++ crosses the host boundary — the macro emits the
same five C symbols. See `plasma/plasma.cpp`.

### API docs

Doxygen covers the whole `include/rgbx/` surface:

```bash
doxygen fw/extensions/Doxyfile     # output: fw/build/doxygen/html
```

## Building

In-repo extensions (each `fw/extensions/<name>/` with a single `.c` or `.cpp`):

```bash
fw/extensions/build.sh            # outputs fw/build/extensions/<name>.llext
```

The script regenerates the LLEXT EDK from the current proto0 build (deleting
the stale tarball first — the `llext-edk` cmake target does not track header
changes), compiles each extension with the EDK's `LLEXT_CFLAGS`, and
partial-links (`ld -r`) the object. The `ld -r` step matters for C++: COMDAT
group sections otherwise interleave with `.data`/`.bss` in file offsets and
trip the llext loader's region-overlap check.

**Third-party / standalone development does not use this script or the EDK.**
Fork the [rgbx-extension-template](https://github.com/skalldri/rgbx-extension-template)
repo instead: it builds the same single translation unit to both a device
`.llext` and a simulator `.wasm` using the `rgbx-sdk` tarball attached to
firmware releases **fw-v3.0.0 and later** (earlier releases carry no SDK
asset — a template pin on one fails at the download step). The submission
flow lives in the root-level `extensions/README.md` (next to
`extensions/registry.json`), the design in
`fw/docs/standalone-extension-repos.md`; the worked example is
[rgbx-demo-wave](https://github.com/skalldri/rgbx-demo-wave).

The `llext-edk.tar.xz` CI artifact that build.yaml still uploads is
**deprecated as a third-party build path**: it carries none of the SDK's
build gates, so a C++ TU built without the `ld -r` step is rejected
on-device (`Region 0 ELF file range ... overlaps with 1`) and a `sinf()`
call compiles but fails llext symbol resolution at load. If you were
building against the EDK, switch to the template/SDK flow above.

## Simulating without hardware

The WASM simulator (`fw/sim/` — full docs in `fw/sim/README.md`) runs an
extension's actual source with the firmware's tick semantics (nominal 11 ms
ticks, 25 Hz IMU / 31.25 Hz audio cadence, color-mode resolution, brightness
truncation, dead-pixel mask, fault handling) and the **real** audio DSP
compiled to WebAssembly:

```bash
fw/sim/rgbx-sim run <name> --scenario metronome-120 --json   # headless, seconds
fw/sim/rgbx-sim serve                                        # browser UI, live mic/IMU
```

It needs no proto0 build, no board, and no locks — use it as the iteration
loop, then do the ARM build below before calling anything done (the sim links
libc/libm statically, so code that fails llext symbol resolution on-device,
like `sinf()`, still runs in the sim — `fw/sim/PARITY.md` has the full
divergence list).

## Installing on the device

You don't have to build the in-repo extensions yourself: prebuilt `.llext` files
are attached to every firmware (`fw-vX.Y.Z`) GitHub release, alongside the
firmware zips they were built with — download the ones matching your installed
firmware. (An extension is only accepted by firmware with the same
`RGBX_ABI_VERSION` and display dimensions, so always take firmware + extensions
from the same release.) CI also uploads them as an `extensions-proto0` workflow
artifact on every firmware build.

```bash
# Mount the board's USB mass-storage disk (see fw/CLAUDE.md "USB Flash Disk"),
# then:
cp fw/build/extensions/plasma.llext /mnt/sunglasses-fs/ext/
sync && umount /mnt/sunglasses-fs
# Reboot the board (kernel reboot warm) so the firmware re-mounts FAT and
# re-discovers extensions.
```

## Debug shell

```
ext list                      # slots, ids, names, [active]/[FAULTED] flags
ext select <slot>             # activate an extension animation (clears a fault)
ext param <slot> <idx> [<v>]  # get/set a param (bools 0/1, strings as text)
ext stats                     # per-extension timing, cpu and wall rows (us)
```

`ext stats` prints a `cpu` row and a `wall` row per slot. The **cpu** row is the
extension's own cost and is what the per-tick budget is enforced against; the
**wall** row additionally contains whatever preempted the sandbox and will run
higher under load, by design (issue #276). Read cpu when judging an extension.

`hello` doubles as the sandbox-recovery test: its `Crash` bool makes the next
tick MPU-fault; `Hang` makes it spin until it exceeds its CPU budget. `Crash` is
reported as soon as the sandbox thread is seen dead; `Hang` takes as long as the
sandbox needs to actually burn `CONFIG_APP_EXT_TICK_CPU_BUDGET_MS` of CPU, so on
a loaded system the fault banner appears later than the budget itself (a budget
is spent at the rate the scheduler grants it — the wall backstop bounds the
tail). Both abort only the
sandbox thread, unload the extension, push Is Active = false to the app, and
scroll the fault banner; `ext select <slot>` clears the fault and retries
(BLE activation of a faulted extension is rejected, so recovery is always a
deliberate action).
