# Animation Extensions

> **New here? Start with [Getting started: your first extension](getting-started.md)** —
> a start-to-finish walkthrough that builds a working C or C++ extension and
> covers parameters, sensor inputs, drawing, and installing it. This page is
> the reference-level detail behind it.

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

[`rgbx_api.h`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/include/rgbx/rgbx_api.h)
(flat C, ABI v1) is the whole contract. An extension
exports five symbols — a `rgbx_manifest` (name, framebuffer dims, parameter
table), a writable `rgbx_inputs` block the host fills before each tick, a
`rgbx_framebuffer` it renders into, and `rgbx_init`/`rgbx_tick` functions —
and calls at most the firmware's exported support surface: the string/memory
functions + `printk`/`vprintk`, plus single-precision libm (`sinf`, `cosf`,
`atan2f`, `sqrtf`, …), the 64-bit integer-division helpers, and `memmove`
([`extension_exports.c`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/src/extensions/extension_exports.c),
issue #295 — the authoritative list is
[`allowed-symbols.txt`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/sdk/arm/allowed-symbols.txt),
and the build gates enforce it). Anything
else — notably all double-precision math — fails symbol resolution at load.
Include [`rgbx_sys.h`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/include/rgbx/rgbx_sys.h)
for the declarations rather than writing your own prototypes: a wrong one links
anyway and then diverges between the device and the simulator (issue #351).
See the template's
[`src/main.c`](https://github.com/skalldri/rgbx-extension-template/blob/main/src/main.c)
for a complete raw-C extension exercising the full surface.

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

Raw-C extensions define + `EXPORT_SYMBOL` it themselves — see
[Getting started](getting-started.md) for a worked example, or the template's
[`src/main.c`](https://github.com/skalldri/rgbx-extension-template/blob/main/src/main.c).
C++ wrapper extensions get it for free: `RGBX_ANIMATION()` always emits the
symbol, driven by the `rgbx::Animation::goodMoment()` virtual (default `true` —
override it to signal real boundaries).

### Parameters

Up to `RGBX_MAX_PARAMS` (16) parameters, each surfaced as a BLE
characteristic with the same presentation format the built-ins use:

| Type                | App control   | Value                                        |
| ------------------- | ------------- | -------------------------------------------- |
| `RGBX_PARAM_UINT32` | number field  | `rgbx_inputs.params[i]`                      |
| `RGBX_PARAM_COLOR`  | color picker  | `params[i]` as `0x00RRGGBB`                  |
| `RGBX_PARAM_BOOL`   | toggle        | `params[i]` as 0/1                           |
| `RGBX_PARAM_STRING` | text field    | `rgbx_inputs.param_strings[s]` (see below)   |
| `RGBX_PARAM_FLOAT`  | decimal field | `params[i]` as IEEE-754 float32 bits — read via `paramF32(i)` |

Declare them with `RGBX_PARAM(name, type, default)` /
`RGBX_PARAM_STR(name, "default")` / `RGBX_PARAM_F32(name, default)`. String
values are capped at `RGBX_PARAM_STRING_MAX-1` (31) bytes, at most
`RGBX_MAX_STRING_PARAMS` (4) per extension; the *i-th string-typed param in
declaration order* reads from `param_strings[i]` (the C++ wrapper's
`paramString(index)` does this mapping for you).

`RGBX_PARAM_FLOAT` values ride in the shared `params[i]` slot as their raw
bit pattern — always read them with the C++ wrapper's `paramF32(i)` (or an
equivalent `memcpy` bit-cast); an integer conversion silently yields
nonsense. An extension declaring a FLOAT param loads only on firmware ≥ the
release that introduced the type — older hosts reject the whole manifest
with "bad param type" (there is no per-param degrade path).

### Inputs

The host snapshots every source into `rgbx_inputs` before each tick (absent
sources read as zeros):

- **IMU** — `accel[3]` (m/s²), `gyro[3]` (rad/s).
- **Audio** — `audio_band_energy[4]` + `audio_beat[4]` (beat detector) and
  `audio_display_bucket[20]` (raw FFT power per spectrum bucket, spanning
  ~60 dB from bass to treble — map through `rgbx/rgbx_audio_bars.h`'s
  `rgbx_audio_bar_height()` for bar graphs; it is the same dB window the
  built-in FFT Bars animation uses).
- **Buttons** — `buttons_pressed` bitmask, pressed-since-last-tick; proto0:
  bit 0=Up, 1=Left, 2=Right, 3=Down, 4=Wake.

### C++ wrapper

C++ authors can use
[`rgbx_animation.h`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/include/rgbx/rgbx_animation.h)
instead: subclass
`rgbx::Animation`, then instantiate with `RGBX_ANIMATION(Class, "Name", W, H,
RGBX_PARAM(...))`. It adds typed accessors (`paramU32/paramColor/paramBool/
paramString`, `bandEnergy/isBeat/displayBucket`, `buttonWasPressed`, accel/
gyro getters). Nothing C++ crosses the host boundary — the macro emits the
same five C symbols. [Getting started](getting-started.md) builds one
step by step; the template's
[`cpp-waves`](https://github.com/skalldri/rgbx-extension-template/tree/main/examples/cpp-waves)
example and the registry-shipped
[rgbx-plasma](https://github.com/skalldri/rgbx-plasma) are further reading.

### API docs

Every type, macro and function in `include/rgbx/` is documented in the
generated reference at <https://rgb-sunglasses.autom8ed.com/api>. To build it
locally:

```bash
fw/extensions/build-docs.sh          # output: fw/build/doxygen/html
```

**Linking to a page from outside this repo?** By default Doxygen names a
Markdown page's output file after its source path, so the URL silently 404s if
the file is ever moved. Give the page an explicit label on its first heading and
Doxygen uses that instead:

```markdown
# Getting started: your first extension {#getting-started}
```

That page is published at
<https://rgb-sunglasses.autom8ed.com/api/getting-started.html> and stays there
wherever the source lives. Do the same for any page you expect to be linked from
outside. The label must be on the **first** heading with nothing above it — any
content before it, even an HTML comment, makes Doxygen fall back to the
path-derived name.

Note this only covers Markdown pages. The generated pages for headers and C++
classes ([`rgbx_api.h`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/include/rgbx/rgbx_api.h),
`rgbx::Animation`, …) are named from those identifiers, so their URLs change if
the header or class is renamed — nothing here guards that, and an external link
to one would break silently.


## Building

**Start here: fork the
[rgbx-extension-template](https://github.com/skalldri/rgbx-extension-template).**
It builds one translation unit to both a device `.llext` and a simulator
`.wasm` with a single
[`./build.sh`](https://github.com/skalldri/rgbx-extension-template/blob/main/build.sh),
needs no firmware checkout and no Zephyr
toolchain, and applies the same build gates CI does. It uses the `rgbx-sdk`
tarball attached to firmware releases **fw-v3.0.0 and later** (earlier releases
carry no SDK asset — a template pin on one fails at the download step). To
publish what you build, see
[Community extension registry](../../extensions/README.md); the worked example
is [rgbx-demo-wave](https://github.com/skalldri/rgbx-demo-wave).

Do **not** build against the `llext-edk.tar.xz` CI artifact that
[`build.yaml`](https://github.com/skalldri/rgb-sunglasses/blob/main/.github/workflows/build.yaml)
still uploads. It is deprecated as a third-party path and carries none of the
SDK's build gates, so the two failures it lets through only surface on-device,
with errors that look unrelated to the build you did:

- a C++ translation unit built without the `ld -r` step is rejected by the
  loader with `Region 0 ELF file range ... overlaps with 1`;
- a call to any symbol outside the exported surface (double-precision `sin()`,
  `sinhf()`, `strtol()`, …) compiles fine and then fails llext symbol
  resolution at load.

If you were building against the EDK, switch to the template/SDK flow above —
it applies both gates at build time.

### In-repo extensions (contributors only)

Extensions living in this repo — each `fw/extensions/<name>/` with a single
`.c` or `.cpp` — build with the EDK instead of the SDK:

```bash
fw/extensions/build.sh            # outputs fw/build/extensions/<name>.llext
```

The script regenerates the LLEXT EDK from the current proto0 build (deleting
the stale tarball first — the `llext-edk` cmake target does not track header
changes), compiles each extension with the EDK's `LLEXT_CFLAGS`, and
partial-links (`ld -r`) the object. The `ld -r` step matters for C++: COMDAT
group sections otherwise interleave with `.data`/`.bss` in file offsets and
trip the llext loader's region-overlap check.

## Simulating without hardware

The WASM simulator runs an extension's actual source with the firmware's tick
semantics (nominal 33 ms ticks, 25 Hz IMU / 31.25 Hz audio cadence, color-mode
resolution, brightness truncation, dead-pixel mask, fault handling) and the
**real** audio DSP compiled to WebAssembly. It needs no board — build a `.wasm`
and drag it onto <https://rgb-sunglasses.autom8ed.com/sim/>, or run it from a
checkout of this repo:

```bash
fw/sim/rgbx-sim run <name> --scenario metronome-120 --json   # headless, seconds
fw/sim/rgbx-sim serve                                        # browser UI, live mic/IMU
```

Use it as the iteration loop — but **a green simulator run does not mean the
extension loads on the device.** The sim links libc/libm statically, so a call
outside the firmware's exported surface (double-precision `sin()`, `strtol()`,
…) runs fine there and then fails llext symbol resolution on-device. Always do
a device build before calling an extension done. (In-repo:
[`fw/sim/README.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/sim/README.md)
and
[`fw/sim/PARITY.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/sim/PARITY.md)
document the simulator and the full divergence list.)

## Installing on the device

Copy the `.llext` into `/NAND:/ext/` on the board's USB mass-storage disk, then
reboot so the firmware re-mounts the filesystem and rescans:

```bash
# Mount the board's USB mass-storage disk (see fw/CLAUDE.md "USB Flash Disk"),
# then:
cp <your-extension>.llext /mnt/sunglasses-fs/ext/
sync && umount /mnt/sunglasses-fs
# Reboot the board (kernel reboot warm) so the firmware re-mounts FAT and
# re-discovers extensions.
```

An extension is only accepted by firmware with the same `RGBX_ABI_VERSION` and
display dimensions, so build against the SDK from the release you are running.

You usually don't have to do any of this by hand: extensions in the
[community registry](../../extensions/README.md) are rebuilt from their pinned
revision on every `fw-v*` release, attached to it as `.llext` assets, and
installed onto the device by the companion app automatically. **The in-repo
extensions are not published that way** — `hello` and `cpptest` are dev/debug
tools that CI builds as a check and no release ships (see the comment in
[`release.yaml`](https://github.com/skalldri/rgb-sunglasses/blob/main/.github/workflows/release.yaml)),
so the manual copy above is the route for anything you build yourself.

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

### Bound your phase accumulators (issue #304)

This section is the **single source of truth** for the argument-range cliff; other
places ([`extension_exports.c`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/src/extensions/extension_exports.c),
`/add-extension`, the
[rgbx-extension-template](https://github.com/skalldri/rgbx-extension-template)
README) point here rather than restating the numbers.

**The exported trig — `sinf`, `cosf` and `tanf` alike — is cheap only while
`|x| <= 201.06`** (`2^7*(pi/2)`). All three route through picolibc's
`__ieee754_rem_pio2f`, which past that threshold falls into `__kernel_rem_pio2f`, a
multi-precision reduction costing several times more *and getting more expensive as
the argument grows*. So an extension that lets a phase accumulator free-run runs at
full speed for a minute or two and then degrades continuously from there.

That is what issue #304 was: plasma's per-tick cost climbed 3.4 ms -> 25 ms over the
first five minutes of every activation and overran the render interval on essentially
every frame.

**Bound the accumulator.** Wrap it at a period where every rate you use completes a
whole number of cycles — for rates 1.1/0.7/1.7 rad/s that is `20*pi` s, because
11 : 7 : 17 share a common period. Then the wrap is seamless *and* the argument is
bounded for any speed multiplier, because the bound is on the accumulator rather than
on the rate.

If your rates share no common period, reduce each phase instead — but note
**`fmodf` keeps the dividend's sign**, so `fmodf(phase, kTwoPi)` yields
`(-2*pi, 0]` for a phase that can go negative (a direction toggle, an IMU-driven
angle). That is fine for feeding `sinf`/`cosf` directly, which accept negative
arguments — [`tilt_animation.cpp:127`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/src/animations/tilt_animation.cpp#L127)
relies on exactly that. It is **not** fine if
you then index a table with the result: copy `wrapPos()`
([`tilt_animation.cpp:89`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/src/animations/tilt_animation.cpp#L89)),
which adds the period back when the result is negative. Getting this wrong converts
a negative float to a huge `size_t`, reads outside the sandbox's MPU region, and
faults the extension.

**Detecting it.** Two signals, cheapest first:

- **Watch the console for `Render overran the tick interval N time(s) ...`**
  ([`pattern_controller.cpp`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/src/pattern_controller.cpp),
  rate-limited to one line per 5 s). This is the direct
  symptom and it costs nothing to notice — during #304 it was printing continuously
  while `ext stats` still looked unremarkable.
- **`ext stats`, read after minutes rather than seconds.** `min`/`avg`/`max`
  accumulate from activation and are **reset on every activation** — not just the
  `ext select` shell command, but any app/BLE animation switch, a shuffle rotation,
  or the boot-time restore
  ([`extension_host.cpp`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/src/extensions/extension_host.cpp)).
  So a reading taken shortly
  after *anything* re-activated the extension only shows the fast phase, and a single
  late reading averages the fast and slow phases together. Sample repeatedly across
  one uninterrupted activation and look for a trend.

Note the CPU budget (`CONFIG_APP_EXT_TICK_CPU_BUDGET_MS`, 50 ms) will **not** catch
this: it is still ~1.5x the 33.3 ms render interval (issue #376 default), so an
extension can miss every frame without ever faulting. #304 peaked at 46.5 ms — against
the 11.1 ms interval of its day — and never tripped it.

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

### Resolving a crash to a source line

A shipped `.llext` carries no DWARF — the SDK strips it, and the release publishes
the unstripped partial link beside each asset as **`<name>.llext.debug`** (PR #429).
When the sandbox faults (an MPU violation, a null call, a divide by zero, ...), the
firmware captures the faulting PC and LR from the exception frame, works out which
of the extension's sections each one landed in, and latches the result in the
slot's `ext faults` record along with a recipe:

```
[2] 'Hello Extension': sandbox thread died mid-tick (CPU fault inside the extension)
      at 56328 ms uptime (15290 ms ago), 1 time(s) since clear
      cpu 0 us / wall 1609253 us  (budget 50000 us / backstop 500000 us)
      crashed: reason 19 (MemManage fault: data access outside the sandbox)
      pc 0x2004291c = .text+0x11c
      lr 0x00027350  (outside the extension — a firmware export or the sandbox entry; resolve against zephyr.elf)
      resolve with the release's hello.llext.debug sidecar:
        arm-none-eabi-addr2line -f -j .text -e hello.llext.debug 0x11c
      params reset to manifest defaults: yes
      currently: FAULTED
```

(That is a real record — `hello` with its `Crash` param set, proto0, 2026-09-02.
The recipe line answers `rgbx_tick` / `hello.c:137`, the deliberate kernel-SRAM
write; the LR resolves against `zephyr.elf` to the host's `sandbox_entry`, which is
what a crash directly inside `rgbx_tick` always looks like.)

Run the printed line against the sidecar from the **same release** the device is
running (`gh release download fw-vX.Y.Z -p 'hello.llext.debug'`) and it names the
function and `file:line` for every in-extension address. The same information is in the
`LOG_ERR` at fault time, but the latched record survives the UART backlog scrolling
away, which is the whole point of `ext faults`.

Two things to know when reading it:

- **The offsets are section-relative, never the runtime address.** The sidecar is
  an `ld -r` relocatable, so its DWARF addresses count from the start of each
  section; that is why the recipe passes `-j .text` with `0x11c`, not `0x2004291c`.
  Feeding addr2line the raw PC returns `??:0`.
- **An address marked "outside the extension" belongs to `zephyr.elf`, not the
  sidecar.** The LR is outside whenever the crash is directly in `rgbx_tick` or
  `rgbx_init` (the caller is the host's sandbox entry, as above). The PC lands
  there when the extension called a firmware export (`memcpy`, `sinf`, `printk`)
  with a bad argument, or jumped through a null or wild function pointer; the LR
  then usually still points into the extension and tells you which call site. For
  the firmware side, `arm-none-eabi-addr2line -e fw/build/fw/zephyr/zephyr.elf
  0x<addr>` against the matching build.

The reason code is Zephyr's `k_fatal_error_reason` (generic 0-4) or the Cortex-M
arch reason (16+); the parenthetical is the readable name. `MemManage fault: data
access outside the sandbox` is by far the most common — the MPU caught a read or
write to memory the sandbox does not own, typically a stale pointer or an
out-of-range table index (see "Bound your phase accumulators" above).
