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

Third-party developers get the same thing from the EDK archive
(`fw/build/fw/zephyr/llext-edk.tar.xz`): extract it, include its
`cmake.cflags` or `Makefile.cflags` (set `LLEXT_EDK_INSTALL_DIR` to the
extracted path), compile your single translation unit with `LLEXT_CFLAGS -c`,
and `ld -r` the result. Both rgbx headers ship inside the archive.

## Installing on the device

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
ext stats                     # per-extension tick-handshake timing (us)
```

`hello` doubles as the sandbox-recovery test: its `Crash` bool makes the next
tick MPU-fault; `Hang` makes it spin until the deadline. Both abort only the
sandbox thread, unload the extension, push Is Active = false to the app, and
scroll the fault banner; `ext select <slot>` clears the fault and retries
(BLE activation of a faulted extension is rejected, so recovery is always a
deliberate action).
