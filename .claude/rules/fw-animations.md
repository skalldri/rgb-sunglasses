---
paths:
  - "fw/src/animations/**"
  - "fw/src/pattern_controller*"
  - "fw/src/led_*"
  - "fw/src/core_config.*"
  - "fw/src/configuration_provider.h"
  - "fw/src/button*"
  - "fw/src/fonts/**"
  - "fw/src/status_led/**"
  - "fw/src/bluetooth/animation_adapters/**"
  - "fw/src/bluetooth/animation_is_active_characteristic.h"
  - "fw/tests/animations/**"
  - "fw/tests/led_controller/**"
  - "fw/tests/configuration_provider/**"
  - "fw/sim/browser/**"
---

# LED rendering, animations and the pattern controller

Loads when you read animation / rendering / pattern-controller code. Adding a built-in animation
is `/add-animation` (several registration spots fail silently if missed).

## LED rendering pipeline

- `fw/src/led_controller.cpp` — manages dual-bank WS2812 LED strip hardware and a
  double-framebuffer. Callers claim a buffer via `claimBufferForRender`, write pixels via
  `set_pixel_in_framebuffer`, then release it.
- `fw/src/pattern_controller.cpp` — sits above the LED controller. Owns the active animation slot
  and an optional `Indicator` overlay (BT advertising/connecting/pairing). Request an indicator
  with `pattern_controller_request_indicator`, switch animations with
  `pattern_controller_change_to_animation`. **`pattern_controller_change_to_animation()` runs
  synchronously on the caller's thread** — BT RX for GATT writes, the shell thread for `anim set`,
  the SMP workqueue for an extension DELETE; only the boot-time switch to the default animation in
  the thread entry runs on the pattern-controller thread itself. Nothing in this file may assume
  pattern-controller-thread context, and no automated gate checks this. The extension host
  serializes its own entry points with a mutex for the same reason.
- `fw/src/led_config.h` — compile-time constants for the frame LED geometry (40×12 logical display
  over two banks, serpentine wiring) and the proto0 onboard status-LED geometry (2×1). All
  rendering code receives a `const LedConfig*` so the same logic runs on any geometry.
- The device boots to the default animation every time; the active animation is deliberately not
  persisted (issue #311, `.claude/rules/fw-settings-persistence.md`).

## Animation system

- `fw/src/animations/animation_base.h` — pure abstract `BaseAnimation` with `init()`, `tick()`,
  `setActive()`.
- `fw/src/animations/animation.h` — `BaseAnimationTemplate<T, A>` CRTP base adding a Meyer's
  singleton (`getInstance()`) and wiring `setActive()` to the registry.
- `fw/src/animations/animation_types.h` — the `Animation` enum (ZigZag, Text, Rainbow, MyEyes,
  Beat, FftBars, GlimPlayer, MatrixCode, Tilt, Pulse, the Bt* indicator states, …). Read it for the
  current list and ids.
- `fw/src/animations/animation_registry.{h,cpp}` — runtime map of `Animation` → factory function +
  optional is-active setter callback. BT-free. Populated by `animation_registry_register_defaults()`.
  **Registration order matters and returns must be checked**: `animation_registry_register_is_active()`
  returns `-ENOENT` unless `animation_registry_register()` already created the id's entry — an
  ignored return here silently killed the extensions' entire Is Active read/notify path on PR #89
  (invisible to every build/test/shell gate; only a real app connection exposed it).
- `fw/src/animations/animation_registry_defaults.cpp` — registers all animations and calls each
  animation's `bind_default_dependencies()` helper; each built-in is gated by its own
  `CONFIG_ANIMATION_<NAME>` symbol in `fw/Kconfig` (Text is always compiled).
- Each parameterized animation has a dependency struct holding `const` references to
  `AnimationUint32ParameterSource` (or similar abstract interfaces). `tick()` reads parameters only
  through these interfaces, keeping animation logic BT-free.
- `fw/src/animations/animation_is_active_binding.h` — BT-free template that bridges the registry's
  `setActive` callback to a GATT characteristic setter and routes remote BLE writes back to
  `pattern_controller_change_to_animation`.
- `fw/src/animations/bt_animations.{h,cpp}` — the visual BT-status indicators (advertising pulse,
  connecting flash, pairing code). BT-themed by design but driven externally via `BtStateObserver`.

## Animation / BT decoupling — COMPLETE

The `animation-refactor-part2` decoupling is **done**: every parameterized animation's GATT
service, parameter sources and is-active wiring live in an adapter under
`fw/src/bluetooth/animation_adapters/` (one per parameterized animation), and no animation `.cpp`
includes BT headers. Keep it that way — new animations get a BT-free `.cpp` plus an adapter.
Verify from the repo root:

```bash
grep -rlE 'bluetooth|BT_GATT|BtGatt' fw/src/animations/
# Matches only comments (adapter cross-references, a BtGattString size note, the app-constants
# cross-reference in color_mode_source.h) — no code.
```

`bt_animations.{h,cpp}` stay in `fw/src/animations/` intentionally — they render BT state but
contain no BT includes. Do not "fix" or relocate them. `fw/docs/animation-bluetooth-decoupling-plan.md`
is the historical plan, now executed.

## DI interfaces

- `fw/src/configuration_provider.h` — `ConfigurationProvider`: abstract interface over the
  `CoreConfig` singleton (getBrightnessFactor, getDisplayRateMs, getRenderRateMs). `CoreConfig`
  (`fw/src/core_config.cpp`: brightness, display/render rates, status-LED brightness, each a
  `BtGattPersistentCharacteristic`) inherits from it. Injected into `led_controller` and
  `pattern_controller` via setters; lazy fallback to `CoreConfig::getInstance()` if not set.
- **`CoreConfig` getters are non-const.** `getBrightnessFactor()` writes back to clamp the value
  against the BT characteristic range, so any abstract interface it implements must declare those
  methods without `const`, or `CoreConfig` becomes abstract and `Singleton<CoreConfig>` fails to
  instantiate.
- `fw/src/button_event_listener.h` + `fw/src/buttons.h` — `ButtonEventListener`:
  `onButtonPressed(size_t buttonId)`. In `fw/src/buttons.cpp` the GPIO callback runs in ISR context; dispatch is deferred
  GPIO interrupt → `K_MSGQ_DEFINE` → `k_work` → listener on the work-queue thread. Register with
  `buttons_register_listener()`. IDs 0–3 = sw0–sw3; ID 4 = wake button.
- **Physical button layout (proto0, a directional grid):** 0 = Up, 1 = Left, 2 = Right, 3 = Down.
  The devicetree labels in `fw/boards/others/rgb_sunglasses_proto0/rgb_sunglasses_proto0_nrf5340_cpuapp_common.dts`
  reflect this (e.g. "Push button 1 (Up)"); the `sw0`–`sw3` aliases are unchanged. Use this mapping
  rather than guessing — e.g. `GlimPlayerAnimation` (`fw/src/animations/glim_player_animation.cpp`) uses 0 (Up) for the next GLIM file and 3 (Down)
  for the previous one; 1/2 are intentionally unassigned there.
- `fw/src/fonts/` — `FontAtlas` and `FontShell`, bitmap font rendering for `TextAnimation` and
  `BtPairingAnimation`.

## Brightness: draw near full-scale

**Animations must render near full-scale (255) channel values**: the pattern controller multiplies
every pixel by the global brightness factor (default 20/1000 = 0.02), so a "dim" animation drawing
at 32/255 is invisible on the panel. This looked like a crash on the original hello extension demo
— it was just arithmetic.

**Corollary — a color's hue visibly drifts as an animation dims it, and that is NOT a bug in the
animation.** The same 0.02 factor means a full-scale channel reaches the strip as ~5, so an
animation that also scales by its own envelope (e.g. `PulseAnimation`'s triangle-wave brightness)
is working with 0-5 PWM levels. Integer truncation kills the smaller channels first: a saturated
pink `(255,0,129)` renders `(5,0,2)` at the peak and degrades `(2,0,1)` → `(1,0,0)` — i.e. **pure
red** — at the dim end, so it reads as "fading between pink and red". Confirmed on hardware (issue
#259) by writing the same color as a plain Static value: identical shift, no color mode involved.
Pastels hide it (their channel ratios survive truncation); fully-saturated hues, which is exactly
what `anim_color_from_hue()` emits, show it most. Reproduce with a Static color before blaming
animation or color-mode logic.

## GLIM player — trying out new GLIM content

`GlimPlayerAnimation` (`anim set glim_player`) replaced the old per-file `bad_apple`/`nyan_cat`
animations. It enumerates every `.glim` file under `/NAND:/glim` on boot (`glim_registry`) and can
play any of them, picked via BLE (a generic "drop-down list" characteristic, see
`glim_player_animation_bt.cpp`), the `glim` shell command (`glim list` / `glim select <index>` /
`glim get_selected` / `glim set_loop_mode <mode>`), or a button press (Up = next, Down =
previous). It reads geometry/frame-count/pixel-format (mono `Raw` or `Rgb24`) from each file's own
header rather than hardcoding anything, so trying new footage is just dropping a new `.glim` into
`/NAND:/glim/` and resetting — no firmware rebuild. Generating assets and copying them to the
board: `.claude/skills/provision-device/references/nand-disk.md`. Format:
`fw/src/storage/GLIM_FORMAT.md`.
