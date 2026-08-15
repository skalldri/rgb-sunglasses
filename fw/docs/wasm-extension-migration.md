# WebAssembly extension migration status

This ledger tracks the effects that must move from the LLEXT extension path to
the RGBX v2 WebAssembly path. Built-in firmware animations are not extension
migration targets.

## Current score

- Behaviorally ported to the RGBX v2 prototype: **1 of 4 (25%)**.
- Production-loadable RGBX packages: **0 of 4**.
- Shipped community effects migrated: **0 of 2**.

`cpptest` is the first behaviorally complete port. It is intentionally enabled
only in the focused QEMU test profile. The firmware application still runs the
embedded `rgbx_mvp.fill` demonstration and the production extension path is
still LLEXT.

The production profile does not define `CONFIG_APP_WASM3_V2_PROTOTYPE`, and
its ELF contains no v2 runtime or import symbols. The verification build uses
829,204 of 900,608 flash bytes and 420,448 of 450,560 RAM bytes, leaving 71,404
flash bytes and 30,112 RAM bytes free. It remains above the 64 KiB flash and 24
KiB RAM stop-loss limits, but no device was flashed for this test-only port.

## Effect inventory

| Effect | Role | RGBX v2 status | Remaining work |
| --- | --- | --- | --- |
| `cpptest` | In-repo development and C++ integration fixture | Behaviorally ported. QEMU matches the legacy integer plasma output for state, speed, color, and invert changes. | Reproducible phone compiler lowering, RGBX packaging, production loader integration, proto0 timing. |
| `plasma` | Registry-shipped community effect | Not ported. | Scalar parameters are covered, but its three `sinf` evaluations per pixel need a bounded math strategy and golden parity tests. |
| `demo_wave` | Registry-shipped community effect | Not ported. | Needs bounded sine, audio display buckets, beat inputs, and `set_good_moment`. |
| `hello` | In-repo development and sandbox recovery fixture | Not ported. | Needs string parameters, buttons, IMU, audio buckets and beats, good-moment signaling, and replacements for deliberate crash and hang test hooks. |

`fw/sdk/tests/consumer/mathtest.c` is not a user-visible effect and is not part
of the four-effect denominator. Its symbol and toolchain coverage still needs a
WebAssembly replacement before the old SDK and LLEXT tooling are removed.

## First port contract

The focused prototype accepts a deliberately small, memoryless profile:

- required exports: `rgbx_init()` and `rgbx_tick(dt_ms)`;
- exact imports: `rgbx_v2.param_u32(id)` and
  `rgbx_v2.set_span8(first_pixel, color0, ..., color7)`;
- `set_span8` calls must cover linear pixel offsets `0, 8, ..., 472` in order;
- a frame commits only after exactly 60 valid spans produce all 480 pixels;
- no linear memory, table, start function, data segments, or element segments;
- at most eight functions, eight numeric globals, and 32 locals per function;
- a separate 250 ms activation compilation budget and the unchanged 50 ms
  steady-state tick CPU budget.

The eight-pixel primitive is a measured requirement, not an aesthetic choice.
A faithful port using 480 `set_pixel` crossings exceeded the 50 ms QEMU tick
budget at about 55 ms. Replacing those with 60 spans but leaving interpreted
helper calls also measured about 55 ms. Compiler inlining plus the span ABI
passes the same 50 ms gate without allocating a 64 KiB WebAssembly memory page.

## What counts as complete

A behavioral port is complete when its admitted RGBX v2 guest matches the
legacy effect's framebuffer, state progression, parameter behavior, and
good-moment behavior under focused tests while respecting resource limits.

A production migration additionally requires:

1. deterministic on-phone source lowering and package emission;
2. firmware validation of the exact emitted profile;
3. the staged RGBX package connected to the production loader and animation
   registry;
4. phone-to-firmware deployment and activation;
5. proto0 CPU, flash, RAM, stack, and visual verification;
6. one compatibility release with old LLEXT packages ignored by the new
   runtime, followed by deletion of LLEXT host, SDK, simulator, and release
   paths.

## Recommended order

1. Turn the `cpptest` lowering into a deterministic compiler fixture and load
   its staged RGBX package through the production candidate path.
2. Add bounded math and port the shipped `plasma` effect.
3. Add audio inputs and good-moment signaling, then port `demo_wave`.
4. Add the remaining development-only inputs and sandbox test replacements,
   then port `hello`.
5. Run the compatibility train and remove LLEXT support.
