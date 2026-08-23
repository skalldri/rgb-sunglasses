# WebAssembly extension migration status

This ledger tracks the effects that must move from the LLEXT extension path to
the RGBX v2 WebAssembly path. Built-in firmware animations are not extension
migration targets.

## Current score

- Behaviorally ported to the RGBX v2 prototype: **2 of 4 (50%)**.
- Production-loadable RGBX packages: **0 of 4**.
- Shipped community effects migrated: **1 of 2**.

`cpptest` and the registry-pinned `plasma` effect are behaviorally complete
ports. They are intentionally enabled only in the focused QEMU test profile. The firmware application still runs the
embedded `rgbx_mvp.fill` demonstration and the production extension path is
still LLEXT.

The checked `cpptest_v2_module.h` bytes are a reviewed behavioral fixture, not
yet reproducible compiler provenance for `cpptest_v2.c`. The readable C file
defines the intended lowering, while framebuffer parity proves what the bytes
do. A later compiler-fixture PR must pin the compiler, flags, generator, and
source-to-Wasm comparison before generated effects or production loading rely
on this path.

The production profile does not define `CONFIG_APP_WASM3_V2_PROTOTYPE`, and
its ELF contains no v2 runtime or import symbols. The explicit verification
build with the palette/luma plus input/lifecycle ABI and 2 KiB value/module
buffers uses 847,240 of 900,608 flash bytes and 436,416 of 450,560 RAM bytes,
leaving 53,368 flash bytes and 14,144 RAM bytes free. That is below both the
64 KiB flash and 24 KiB RAM
stop-loss limits, so no board configuration enables this test-only path and no
device was flashed.

## Effect inventory

| Effect | Role | RGBX v2 status | Remaining work |
| --- | --- | --- | --- |
| `cpptest` | In-repo development and C++ integration fixture | Behaviorally ported. QEMU matches the legacy integer plasma output for state, speed, color, and invert changes. | Reproducible phone compiler lowering, RGBX packaging, production loader integration, proto0 timing. |
| `plasma` | Registry-shipped community effect | Behaviorally ported as a firmware conformance fixture. The memoryless guest uses a bounded Q15 recurrence and palette/luma spans; QEMU compares every channel against the legacy three-sine formula within one value. | Move the canonical source and deterministic package build to `skalldri/rgbx-plasma`, then consume its registry-pinned revision and digest here. |
| `demo_wave` | Registry-shipped community effect | Not ported. | Needs bounded sine, audio display buckets, beat inputs, and `set_good_moment`. |
| `hello` | In-repo development and sandbox recovery fixture | Not ported. | Needs string parameters, buttons, IMU, audio buckets and beats, good-moment signaling, and replacements for deliberate crash and hang test hooks. |

`fw/sdk/tests/consumer/mathtest.c` is not a user-visible effect and is not part
of the four-effect denominator. Its symbol and toolchain coverage still needs a
WebAssembly replacement before the old SDK and LLEXT tooling are removed.

## First port contract

The focused prototype accepts a deliberately small, memoryless profile:

- required exports: `rgbx_init()` and `rgbx_tick(dt_ms)`;
- exact imports: `rgbx_v2.param_u32(id)` and
  exactly one of `rgbx_v2.set_span8(first_pixel, color0, ..., color7)` or
  `rgbx_v2.set_luma_span8(first_pixel, foreground, background, luma0, ..., luma7)`;
- `set_span8` calls must cover linear pixel offsets `0, 8, ..., 472` in order;
- a frame commits only after exactly 60 valid spans produce all 480 pixels;
- no linear memory, table, start function, data segments, or element segments;
- at most eight functions, eight numeric globals, and 32 locals per function;
- a separate 250 ms activation compilation budget and the unchanged 50 ms
  steady-state tick CPU budget.

The palette/luma primitive accepts only luma values from 0 through 255 and
interpolates each RGB channel with signed integer deltas. It preserves the same
ordered 60-span, complete-frame transaction as direct RGB output while reducing
repeated guest palette math for bounded effects such as Plasma. A module may
import one span encoding, never both.

The eight-pixel primitive is a measured requirement, not an aesthetic choice.
A faithful port using 480 `set_pixel` crossings exceeded the 50 ms QEMU tick
budget at about 55 ms. Replacing those with 60 spans but leaving interpreted
helper calls also measured about 55 ms. Compiler inlining plus the span ABI
passes the same 50 ms gate without allocating a 64 KiB WebAssembly memory page.

## RGBX v2 input and lifecycle contract

The public `rgbx/rgbx_v2.h` header defines three optional input/lifecycle
imports in addition to the existing parameter and span functions:

- `input_u32(kind, index) -> i32` reads one immutable per-tick snapshot value;
- `set_good_moment(value)` publishes exactly one boolean shuffle boundary;
- `debug_u32(tag, value)` emits at most four numeric diagnostics into the
  bounded host mailbox.

The host implementation must admit at most 64 `input_u32` calls per tick. Its kinds cover four Q16 audio
bands, 20 Q16 display buckets, the beat mask, pressed-button mask, three signed
accelerometer axes in milli-units, three signed gyroscope axes in milli-units,
and the bounded length or byte sum of four NUL-terminated 31-byte string
parameter slots. Kind and index validation is fail-closed. Signed IMU values
preserve their i32 bit patterns.

The immutable package capability mask authorizes input classes independently:
buttons require `RGBX_V2_CAPABILITY_BUTTONS`, both IMU vectors require
`RGBX_V2_CAPABILITY_IMU`, and every audio value requires
`RGBX_V2_CAPABILITY_AUDIO`. String summaries require no sensor capability.
Importing `input_u32` never grants a capability by itself.

Importing `set_good_moment` makes one call mandatory for every successfully
committed frame; omitting it keeps the compatibility default of `true`.
The host must fail closed on duplicate calls, values outside zero or one,
invalid input selectors, quota overruns, excess diagnostics, or wrong import
signatures. A rejected generation cannot change the previous framebuffer,
good-moment value, or diagnostics.

This slice remains memoryless. Bounded linear memory, retained-frame scheduling,
and the stateful Fluid profile are separate follow-ups and are not implied by
the input snapshot contract.

The focused `CONFIG_APP_WASM3_V2_PROTOTYPE` host implements this contract in
the existing `K_USER` sandbox. The per-tick capability mask is copied from the
admitted package metadata and checked before every sensor read. Before Wasm3
parses a v2 module, a bounded section scan admits only Type, Import, Function,
Global, Export, and Code in canonical order; custom, DataCount, tag, unknown,
duplicate, and out-of-order sections fail closed. All 33 ARM/QEMU cases pass.
They synthesize the full five-import
module in the test itself, exercise every input class and both good-moment
values, and prove invalid selectors, missing or duplicate lifecycle signals,
call-quota overruns, diagnostic overflow, and wrong optional signatures fail
without committing any output.

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

1. Move the deterministic RGBX v2 compiler/package workflow into
   `rgbx-extension-template`, then move canonical Plasma source into
   `rgbx-plasma`.
2. Port `demo_wave` against the input and good-moment contract, keeping its
   canonical source in its registry repository.
3. Port the safe portions of `hello`; keep deliberate crash and hang coverage
   in host-owned sandbox fixtures rather than public guest parameters.
4. Add bounded stateful memory and retained frames for Fluid and Metaballs.
5. Connect production loading and staged installation, then remove LLEXT.
