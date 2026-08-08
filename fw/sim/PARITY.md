# Simulator ↔ device parity

What the simulator reproduces exactly, what it approximates, and what it
cannot prove. **The ARM build (`fw/extensions/build.sh`) plus on-device
verification remain mandatory before an extension is "done"** — the sim
proves logic, not linkage or timing.

## Reproduced exactly (bit-for-bit where stated)

| Behavior | How |
|---|---|
| `rgbx_inputs` / `rgbx_manifest` memory layout | wasm32 is ILP32 with natural alignment, same as ARM EABI; `shim/abi_offsets.c` static-asserts every offset at build time and `core/abi.ts` mirrors them (checked by unit test). |
| Tick cadence | `dt_ms` is the same constant nominal 11 the device passes (~90 Hz); it is **not** measured time on the device either. |
| Input cadences | Audio features refresh per 32 ms block (31.25 Hz) and hold between — **including sticky beat flags across ~3 ticks**; IMU refreshes at 25 Hz and holds; buttons are edge-latched bitmask per tick. Same as `extension_host::tick()`. |
| Audio features | The **real** `fw/src/sound/audio_dsp.cpp` + SDK CMSIS-DSP compiled to WASM. Verified vs the native_sim replay harness on a click track with byte-identical PCM input: max scale-relative float difference 3.7e-7, **0/250 beat decisions differ** (`fw/tools/beat_lab/compare_sim.py`, gated in CI). |
| COLOR mode resolution | `core/colorMode.ts` is a semantic port of `ColorModeSource` (Static/SpectrumSweep/RandomOnBeat/RandomOnActivate/RandomTimerFade, hue wheel, ≥60° rolls, Q16 phase, shorter-arc lerp), resolved host-side per tick exactly like `extension_host.cpp:1177-1181` — extensions never see the mode byte. |
| Brightness + hue drift | Global brightness = coreBrightness/1000 (default **0.02**), float multiply then integer **truncation** — reproduces the issue #259 hue drift and the "renders at 32/255 → invisible" trap (machine-checked as `visibleAfterBrightness`). No gamma anywhere, same as the device. |
| Dead pixels | The 48 nose-cutout cells from `led_config.h` / `led_controller.cpp` bank rules are masked in every rendering and stat. |
| Manifest validation | `core/manifest.ts` ports `extension_manifest.cpp` rule-for-rule with identical Result names and check order (name truncation at 24/20 bytes, string defaults rejected at ≥32, slotting). |
| Fault → params reset | Every tick-time fault resets params (and their string slots) to manifest defaults; load/init failures do not; a faulted slot refuses re-activation until cleared. Same rules as `sandbox_fault()`. |
| Lifecycle | Re-activation re-instantiates the module — globals reset every activation, like the device's unload/reload. `_initialize()` (C++ ctors) runs in the sandbox worker, the analog of `llext_bringup()` on the sandbox thread. |

## Approximated (documented differences)

| Divergence | Sim behavior | Device behavior | Consequence |
|---|---|---|---|
| CPU budget | Per-tick **wall time inside the worker** compared to the 50 ms budget, on a CPU ~50-100× faster than the 128 MHz M33. | CPU cycles charged to the sandbox thread (`CONFIG_APP_EXT_TICK_CPU_BUDGET_MS`). | An extension that busts the device budget can pass in sim. Watch `timing.tickWallMs` in the report — >0.5 ms avg in sim is a red flag. Timing must be validated on device. |
| Hang verdict | A spinning `rgbx_tick` cannot be interrupted mid-call; the run lands on `wall_backstop` (worker terminated at 500 ms). | A spin busts the **CPU** budget first (verdict CpuBudgetExceeded, issue #276). | Same recovery behavior (fault + params reset); different verdict *name* for spins. `param-hang` expects `wall_backstop` in sim. |
| Memory protection | No MPU: a wild pointer **inside** linear memory silently corrupts the extension's own data. Out-of-range addresses (e.g. hello's `0x20000000`, far beyond the small linear memory) still trap. Partial mitigation: the manifest struct + param table are diff-checked every tick (`manifestIntact`) — the device keeps them in read-only rodata. | MPU faults on any access outside the extension's 4 regions or writes to rodata. | A buggy extension may "work" in sim and crash on device. |
| Symbol resolution | wasi-sdk links libc/libm statically — **any** libc/libm call works in the sim. The zero-import gate rejects wasm *imports*, not statically-satisfiable libc calls. | The device resolves against its exported set only: Zephyr's string/memory functions + printk/vprintk + the curated `fw/src/extensions/extension_exports.c` surface (single-precision libm incl. `sinf`, 64-bit division helpers, memmove — issue #295, mirrored in `fw/sdk/arm/allowed-symbols.txt`). Anything else — double-precision math, `sinhf`, `strtol`, … — **fails at llext load time**. | This is why the ARM build (and its undefined-symbol gate) stays mandatory: the sim can't tell an exported libm call from an unexported one. |
| printk | Self-contained formatter in `shim/sim_shim.c`: same integer/string subset, and `%f`/`%g` emit the literal specifier — deliberately matching the device (`CONFIG_CBPRINTF_FP_SUPPORT=n`). Buffer is 2 KB per tick, overflow drops (device: UART never drops but interleaves). | cbprintf → UART console. | Log content matches for the supported specifiers. |
| RNG | Seeded mulberry32 (reproducible runs, golden tests). | `sys_rand32_get` (hardware entropy). | Random color-mode hue *sequences* differ; semantics (≥60° distance) identical. |
| Float arithmetic | wasm IEEE-754 (deterministic across platforms/browsers). libm (`cosf` in the Hann window, `log1pf` in flux) is wasi-libc. | ARM VFMA contraction + newlib libm. | Audio features differ from the device in late bits (see the measured 3.7e-7 above vs native_sim; device-vs-native_sim has the same class of difference — `fw/tools/beat_lab/compare.py`). Beat decisions on borderline frames can flip. |
| Audio input level | Browser mic / WAV levels are line-level; there is **no AGC loop** (the device's PDM gain servo). Band energies are raw mean power either way (consumers scale by energyScale ≈ 20). | PDM capture + AGC holding RMS in a target window. | Absolute energy magnitudes from live mic may sit in a different range than a well-AGC'd device signal. Use device WAV recordings (`sound mic record_wav`) or the metronome scenarios for calibrated stimuli. |
| Display refresh | Render 90 Hz; browser canvas vsyncs, CLI has no refresh. Optional 30 Hz decimation toggle mirrors the LED strip rate. | Strip updates at 30 Hz while render runs 90 Hz. | Cosmetic. |

## Not covered at all — device-only

- llext loading: ELF relocation, region-overlap check (`ld -r` requirement),
  the 24 KB `CONFIG_LLEXT_HEAP_SIZE` fit, symbol resolution against the real
  export table.
- MPU sandboxing and `k_sys_fatal_error_handler` behavior.
- BLE: the runtime GATT service, param persistence blobs, Is Active notify,
  metadata blob.
- Scheduling: sandbox thread priority 9 starvation effects, lock contention
  with BT RX writes.
- Power, real PDM/AGC, real BMI270 sample timing jitter.

## Verification ladder

1. `rgbx-sim run <ext> --scenario <s>` — logic, rendering, input handling,
   fault behavior (seconds).
2. `fw/extensions/build.sh` — ARM compile + the device's real linker
   pressure (only the exported symbol surface, no heap).
3. `/flash-and-verify` + `ext select` on the board — load, MPU, timing,
   the real panel.
