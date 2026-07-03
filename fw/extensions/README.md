# Animation Extensions

Loadable animation extensions (`.llext` files) run **fully sandboxed**: the
firmware executes extension code exclusively on a dedicated user-mode thread
confined to the extension's own MPU memory domain (GitHub issue #85). A buggy
extension can hang or crash *itself* — the firmware aborts the sandbox on a
missed tick deadline or MPU fault, keeps running, and the extension can be
re-activated from the app or shell.

Extensions are discovered at boot from `/NAND:/ext/*.llext` and appear as
**first-class animations**: they get their own BLE GATT service (Animation
Name + Is Active + one characteristic per declared parameter) that the
companion app renders exactly like a built-in animation, with zero app-side
changes.

## The ABI

`include/rgbx/rgbx_api.h` (flat C, ABI v1) is the whole contract. An extension
exports five symbols — a `rgbx_manifest` (name, framebuffer dims, parameter
table), a writable `rgbx_inputs` block the host fills before each tick, a
`rgbx_framebuffer` it renders into, and `rgbx_init`/`rgbx_tick` functions —
and never calls into the firmware. See `hello/hello.c` for the minimal raw-C
shape.

C++ authors can use `include/rgbx/rgbx_animation.h` instead: subclass
`rgbx::Animation`, then instantiate with `RGBX_ANIMATION(Class, "Name", W, H,
RGBX_PARAM(...))`. Nothing C++ crosses the host boundary — the macro emits the
same five C symbols. See `plasma/plasma.cpp`.

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

Up to 4 extensions load per boot (`extension_host::kMaxExtensions`), sorted by
filename; each gets animation id `0x20 + slot`.

## Debug shell

```
ext list                      # slots, ids, names, [active]/[FAULTED] flags
ext select <slot>             # activate an extension animation
ext param <slot> <idx> [<v>]  # get/set a manifest parameter without BLE
ext scan                      # re-run discovery (only when nothing loaded)
```

`plasma` doubles as the sandbox-recovery test: setting Speed to `0xDEAD`
makes the next tick MPU-fault; `0xF00D` makes it spin until the deadline.
Both abort only the sandbox thread; re-activate to recover.
