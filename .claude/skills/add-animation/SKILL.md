---
name: add-animation
description: Add a new built-in LED animation to the firmware (enum, animation class, BT adapter, Kconfig, CMake, registry, DI test, shell name). Follow this checklist exactly — several registration spots fail silently if missed.
---

# Add a built-in LED animation

Adding one animation touches ~10 files. The build succeeds even if you miss the
registry, shell, or board-conf spots — the animation just silently doesn't exist at
runtime. Work through **every** step; the full per-file detail (exact code shapes,
grep anchors, checkboxes) lives in [references/checklist.md](references/checklist.md).

The canonical exemplar throughout is **Rainbow** (`grep -rn Rainbow fw/src/` finds
every source-level touch-point; the Kconfig/CMake/test/MCP spots use
`rainbow`/`RAINBOW` instead — those are steps 5–11, or
`grep -rni rainbow fw/ .serial_mcp/` for everything at once). Read `fw/CLAUDE.md`
before starting.

## The 11 steps

1. **Enum** — add `<Name> = <next id>` in `fw/src/animations/animation_types.h`
   **before** the `Count` sentinel (`Count` must stay last; extension static_asserts
   depend on it).
2. **Header** — `fw/src/animations/<name>_animation.h`: a `<Name>AnimationDependencies`
   struct holding `const AnimationUint32ParameterSource&` members, then
   `class <Name>Animation : public BaseAnimationTemplate<<Name>Animation, Animation::<Name>>`
   with `setDependencies()`, `init()`, `tick()` — copy `rainbow_animation.h`. Declare
   `void <name>_animation_bind_default_dependencies();` at the bottom.
3. **Implementation** — `fw/src/animations/<name>_animation.cpp`, strictly BT-free (no
   `bluetooth/` includes). `tick()` MUST guard against unbound dependencies —
   registration order means it can tick before binding. Convention:
   `__ASSERT(deps_, "<Name>Animation::tick before setDependencies");` (see
   `rainbow_animation.cpp`); animations with subsystem sources instead null-check and
   blank the display (see `beat_animation.cpp`, `tilt_animation.cpp`).
4. **BT adapter** — `fw/src/bluetooth/animation_adapters/<name>_animation_bt.cpp`,
   copied from `rainbow_animation_bt.cpp` (the canonical template — all 9 parts are
   itemized in the checklist). Do not restate or re-derive the GATT template
   machinery; for `BtGattServer`/characteristic internals see /add-gatt-characteristic.
5. **Kconfig** — `config ANIMATION_<NAME>` (bool, help text) in `fw/Kconfig`. Add
   `depends on AUDIO` / `depends on IMU` / `depends on FAT_FILESYSTEM_ELM` if the
   animation consumes those subsystems.
6. **CMake** — TWO `target_sources_ifdef(CONFIG_ANIMATION_<NAME> ...)` lines in
   `fw/CMakeLists.txt`: one for the animation `.cpp`, one for the `_bt.cpp` adapter.
   Missing the second builds fine but the animation has no BLE service and never
   registers its Is Active setter.
7. **Registry** — `fw/src/animations/animation_registry_defaults.cpp`, all inside
   `#if defined(CONFIG_ANIMATION_<NAME>)`: include + `using` alias + factory, then in
   `animation_registry_register_defaults()`: `registerActivator(&sActivator)` in the
   top block, then `animation_registry_register(id, factory)` **then**
   `animation_registry_register_is_active(id, ...)` — order matters:
   `register_is_active` returns `-ENOENT` if `register` wasn't called for that id
   first (see `animation_registry.cpp`). **Check BOTH returns** — an ignored return
   here once silently killed the Is Active notify path (caught in PR #89 review;
   invisible to every build/test gate). Finally call
   `<name>_animation_bind_default_dependencies()`.
8. **Enable** — `CONFIG_ANIMATION_<NAME>=y` in
   `fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf` — the FLAT file next to
   `fw/boards/others/`, NOT inside `fw/boards/others/<board>/` (a conf there is
   silently ignored). New features are proto0-only; the DK image is nearly full
   (FLASH ~94% as of 2026-07, PR #89 — re-verify from your own build output).
   `default y` in Kconfig is only OK if it fits on BOTH boards.
9. **DI test suite** — `fw/tests/animations/<name>_animation_di/` (copy
   `rainbow_animation_di`). Full recipe including the exact
   prj.conf/testcase.yaml/CMakeLists incantations: /add-fw-test. No `CONFIG_BT` is
   needed — that old requirement is gone. (Not every animation has a suite —
   `matrix_code` doesn't — but every NEW one must.)
10. **Shell names** — TWO mandatory spots in `fw/src/pattern_controller.cpp`
    (find both via `grep -n '"rainbow"\|(rainbow,' fw/src/pattern_controller.cpp`):
    the `SHELL_SUBCMD_DICT_SET_CREATE(sub_anim_set, ...)` dict AND the
    `cmd_anim_get` switch. Missing either silently breaks `anim set`/`anim get` and
    the MCP tools layered on them. If the animation should be MCP-drivable, also add
    its shell name to `SETTABLE_ANIMATIONS` in `.serial_mcp/plugins/rgb_sunglasses.py`.
11. **Validate** — the ladder: single suite
    (`twister -T fw/tests/animations/<name>_animation_di -p native_sim --outdir fw/twister-out-one`
    — the scratch `--outdir` is /test-fw's targeted-run rule; without it twister
    drops a non-gitignored `twister-out/` at the repo root), then
    /build-proto0 AND /build-dk (both must link; watch DK FLASH%), then /submit-pr.
    On-device verification is /flash-and-verify — not part of this skill. When
    reporting what remains unverified, explicitly include the Is Active notify path
    and GATT service registration: the DI suite is BT-free and never exercises
    either — only the /build-* links plus on-device /flash-and-verify do.

## RENDERING RULES (every tick() must obey)

- **BLE-writable uint32 parameters can arrive with ANY value** — guard zero AND
  extreme magnitudes. Beware intermediate multiplies: uint32 products wrap at 2^32
  on BOTH nRF5340 and native_sim (both 32-bit here), and DI tests use modest values,
  so overflow bugs pass every Twister run — widen to `uint64_t` or clamp before
  scaling math.
- **`setPixel` takes `uint8_t` channels** (`animation_renderer.h`) — compute integer
  values or cast explicitly; don't pass floats.
- **Draw at/near 255.** The BLE brightness factor (`core/brightness`, default
  20/1000 = 0.02) is applied downstream — dim source colors render as *black* and
  have been mistaken for a crash before.
- **Never hardcode 40x12.** Use `renderer.displayWidth()` / `displayHeight()`
  (`fw/src/animations/animation_renderer.h`) — the DK display is 8x2.
- **Never setPixel off-display** (`x >= displayWidth() || y >= displayHeight()`): it
  returns -1 and spams `LOG_ERR` every frame.
- **Nose cutout**: on the glasses frame, pixels with x in [15,25) AND y in [6,12)
  don't exist (`kFrameNoseCutout*` in `fw/src/led_config.h`). Writes there return -2
  *silently* — legal, but don't put important content in that region.
- **Audio/IMU input** goes through the `AnimationAudioSource` /
  `AnimationImuSource` interfaces (`fw/src/animations/animation_*_source.h`) — never
  read the msgqs directly. Call `source.update()` **exactly once** at the start of
  each tick. Bind in `fw/src/sound/animation_adapters/audio_animations_sound.cpp` /
  `fw/src/imu/animation_adapters/imu_animations_imu.cpp`, and extend the `if(...)`
  gate for that adapter file in `fw/CMakeLists.txt` with your CONFIG symbol.

## Scope boundaries

- GATT/characteristic details (persistent characteristics, UUID scheme, CPF,
  metadata blob): /add-gatt-characteristic.
- DI test recipe and twister pitfalls: /add-fw-test.
- Sandboxed `.llext` extension animations (a different mechanism entirely):
  /add-extension.
- Flashing and on-device verification: /flash-and-verify (hardware lock required).
