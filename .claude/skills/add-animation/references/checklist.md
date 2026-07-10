# Per-file checklist: adding a built-in animation

Work top to bottom; open each grep anchor's match in the **Rainbow** exemplar, copy
its shape, rename. `<Name>` / `<name>` / `<NAME>` = PascalCase enum name /
snake_case file+shell name / SCREAMING_SNAKE Kconfig name.

Two dependency shapes exist — pick per parameter kind:

- **Pattern A (BLE uint32 params only)** — a `Dependencies` struct of
  `const AnimationUint32ParameterSource&`, one `setDependencies()`, one bind
  function defined in the `_bt.cpp` adapter. Exemplar: **Rainbow**.
- **Pattern B (audio/IMU/button source, possibly + BLE params)** — individual
  setters (`setAudioSource(...)`, `setColor(...)`) and one bind function **per
  providing subsystem** (`..._bind_default_sound_dependencies()` in the sound
  adapter, `..._bind_default_bt_dependencies()` in the `_bt.cpp` adapter, etc. —
  locations in step 4's Pattern B note).
  Exemplars: **Beat** (audio), **Tilt** (IMU), **GlimPlayer** (buttons).

## [ ] 1. `fw/src/animations/animation_types.h` — enum entry
Anchor: `grep -n "Tilt = 12" fw/src/animations/animation_types.h`

Add `<Name> = <next-integer>,` immediately before the `Count` sentinel; `Count` stays
last (`fw/src/extensions/extension_limits.h` static_asserts extension IDs 0x40+ above it).

## [ ] 2. `fw/src/animations/<name>_animation.h` — class + deps struct
Anchor: `cat fw/src/animations/rainbow_animation.h` (~27 lines; copy wholesale)

Structure per SKILL.md step 2. Exact bits the copy must keep:
- `#include <animations/animation.h>` and `<animations/animation_parameter_source.h>`
  (Pattern B adds `<animations/animation_audio_source.h>` / `animation_imu_source.h`)
- Pattern A: `<Name>AnimationDependencies` members are
  `const AnimationUint32ParameterSource&`, initialized by constructor
- `void init() override;` and `void tick(AnimationRenderer&, size_t timeSinceLastTickMs) override;`
- private `const <Name>AnimationDependencies *deps_ = nullptr;`
- Pattern B: per-source setters instead of `setDependencies(...)`, and one bind
  declaration per subsystem (`..._bind_default_sound_dependencies();` AND
  `..._bind_default_bt_dependencies();` — see `beat_animation.h`)

## [ ] 3. `fw/src/animations/<name>_animation.cpp` — BT-free rendering
Anchor: `grep -n "__ASSERT(deps_" fw/src/animations/rainbow_animation.cpp`

- Verify BT-free (SKILL.md step 3): `grep -n "bluetooth\|BtGatt" fw/src/animations/<name>_animation.cpp` → no hits.
- First line of `tick()` guards unbound deps:
  - Pattern A: `__ASSERT(deps_, "<Name>Animation::tick before setDependencies");`
  - Pattern B: `if (!audioSource_) { /* blank display, return */ }` — copy the loop from `beat_animation.cpp` / `tilt_animation.cpp`.
- Obey EVERY bullet of the RENDERING RULES box in SKILL.md.

## [ ] 4. `fw/src/bluetooth/animation_adapters/<name>_animation_bt.cpp` — the 9 parts
Anchor: `cat fw/src/bluetooth/animation_adapters/rainbow_animation_bt.cpp`
(the canonical template — every part below appears there in this order)

1. **Service UUID**:
   `constexpr bt_uuid_128 k<Name>ConfigServiceUuid = BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::<Name>));`
2. **Primary service**: `BtGattPrimaryService<k<Name>ConfigServiceUuid> <name>PrimaryService;`
3. **One `BtGattPersistentCharacteristic` per parameter**:
   `BtGattPersistentCharacteristic<"<name>/<param>", "<Human Label>", false, uint32_t, <default>> <name><Param>;`
   — args: persisted settings key (stable literal, never derived) + app control label (/add-gatt-characteristic).
4. **Is Active**: `IsActiveCharacteristic<Animation::<Name>> <name>IsActive;`
   (include `<bluetooth/animation_is_active_characteristic.h>`)
5. **Animation Name**:
   `constexpr BtGattString<24> k<Name>AnimationName = makeBtGattString<24>("<Display Name>");`
   then `BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>, k<Name>AnimationName> <name>AnimationName;`
6. **Server + link-time registration**:
   `BtGattServer <name>ConfigServer(<name>PrimaryService, <params...>, <name>IsActive, <name>AnimationName);`
   `BT_GATT_SERVER_REGISTER(<name>ConfigServerStatic, <name>ConfigServer);`
   ⚠️ Argument order is positional and assigns UUIDs — once shipped, NEVER reorder (/add-gatt-characteristic rule 1).
7. **Parameter sources** (anonymous namespace): one
   `class ... : public AnimationUint32ParameterSource { uint32_t get() const override { return <characteristic>; } };`
   per param, plus static instances and a static
   `<Name>AnimationDependencies sDefaultDeps(...);`
8. **Is Active registrar struct** (static-init):
   ```cpp
   using <Name>AnimationIsActive = AnimationIsActiveBinding<Animation::<Name>>;
   static void <name>_set_is_active(bool active) { <name>IsActive.setActive(active); }
   struct <Name>IsActiveBindingRegistrar {
       <Name>IsActiveBindingRegistrar() { <Name>AnimationIsActive::registerSetter(<name>_set_is_active); }
   };
   [[maybe_unused]] <Name>IsActiveBindingRegistrar s<Name>IsActiveBindingRegistrar;
   ```
9. **Bind function definition**:
   ```cpp
   void <name>_animation_bind_default_dependencies() {
       <Name>Animation::getInstance()->setDependencies(sDefaultDeps);
   }
   ```

Pattern B: audio/IMU/button bind functions go next to the existing ones in the subsystem adapters —
`grep -n "bind_default" fw/src/sound/animation_adapters/audio_animations_sound.cpp fw/src/imu/animation_adapters/imu_animations_imu.cpp fw/src/buttons/animation_adapters/button_animation_source.cpp`.

## [ ] 5. `fw/Kconfig` — config symbol
Anchor: `grep -n "config ANIMATION_TILT" -A 6 fw/Kconfig`

```
config ANIMATION_<NAME>
	bool "Enable <Name> animation"
	depends on AUDIO        # only if it uses AnimationAudioSource
	depends on IMU          # only if it uses AnimationImuSource
	depends on FAT_FILESYSTEM_ELM   # only if it reads NAND files
	help
	  One or two sentences: what it draws, what drives it.
```

`default y` ONLY if it fits BOTH boards (SKILL.md step 8); otherwise omit and enable
per-board (step 8 below). Never reuse another subsystem's symbol (fw/CLAUDE.md).

## [ ] 6. `fw/CMakeLists.txt` — BOTH source lines
Anchor: `grep -n "rainbow" fw/CMakeLists.txt` (two hits — you need two as well)

```cmake
target_sources_ifdef(CONFIG_ANIMATION_<NAME> app PRIVATE src/animations/<name>_animation.cpp)
target_sources_ifdef(CONFIG_ANIMATION_<NAME> app PRIVATE src/bluetooth/animation_adapters/<name>_animation_bt.cpp)
```

Put each next to its siblings (animation block ~top, adapter block below it).
Pattern B additionally: extend YOUR subsystem's shared-adapter gate (the wrong gate
leaves your bind function uncompiled — link error far from the edit):

- **Audio** — anchor `grep -n "CONFIG_ANIMATION_BEAT OR" fw/CMakeLists.txt` →
  `if(CONFIG_ANIMATION_BEAT OR CONFIG_ANIMATION_FFT_BARS OR <yours> OR (CONFIG_APP_EXTENSION_HOST AND CONFIG_AUDIO))`
- **IMU** — anchor `grep -n "CONFIG_ANIMATION_TILT OR" fw/CMakeLists.txt` →
  `if(CONFIG_ANIMATION_TILT OR <yours> OR (CONFIG_APP_EXTENSION_HOST AND CONFIG_IMU))`
- **Buttons** — `button_animation_source.cpp` compiles unconditionally; no gate.

## [ ] 7. `fw/src/animations/animation_registry_defaults.cpp` — 3 spots, both returns checked
Anchor: `grep -n "CONFIG_ANIMATION_RAINBOW" fw/src/animations/animation_registry_defaults.cpp`
(4 hits. Rainbow's `#include` is unconditional; guard yours like matrix_code's —
`grep -n "matrix_code_animation.h" -B1 fw/src/animations/animation_registry_defaults.cpp`)

All inside `#if defined(CONFIG_ANIMATION_<NAME>)` guards:

1. **Top of file**: `#include <animations/<name>_animation.h>`, the alias
   `using <Name>AnimationIsActive = AnimationIsActiveBinding<Animation::<Name>>;`,
   and the factory
   `BaseAnimation *<name>_animation_factory() { return <Name>Animation::getInstance(); }`
2. **Activator block** (top of `animation_registry_register_defaults()`):
   `AnimationIsActiveBinding<Animation::<Name>>::registerActivator(&sActivator);`
3. **Registration block** — exact shape, ORDER AND RETURN CHECKS MANDATORY (wrong
   order → `-ENOENT`, anchor `grep -n ENOENT fw/src/animations/animation_registry.cpp`;
   ignored-return incident PR #89 — SKILL.md step 7):

   ```cpp
   #if defined(CONFIG_ANIMATION_<NAME>)
       ret = animation_registry_register(Animation::<Name>, <name>_animation_factory);
       if (ret) {
           return ret;
       }

       ret = animation_registry_register_is_active(Animation::<Name>,
                                                   <Name>AnimationIsActive::setLocalActiveState);
       if (ret) {
           return ret;
       }

       <name>_animation_bind_default_dependencies();   // Pattern B: one call per subsystem
   #endif
   ```

## [ ] 8. `fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf` — enablement
Anchor: `grep -n "CONFIG_ANIMATION_TILT" fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf`

Add `CONFIG_ANIMATION_<NAME>=y` (skip if the Kconfig entry is `default y`). Use the
FLAT `fw/boards/<board_target>.conf`, never `fw/boards/others/<board>/` (SKILL.md
step 8). Shared settings: `fw/prj.conf` (anchor: `grep -n CONFIG_ANIMATION_BEAT fw/prj.conf`).

## [ ] 9. `fw/tests/animations/<name>_animation_di/` — DI test suite
Anchor: `ls fw/tests/animations/rainbow_animation_di/` (CMakeLists.txt, prj.conf,
src/, testcase.yaml)

Full recipe, pitfalls (dotted testcase name, no `CONFIG_BT`), and the
fake-renderer/fake-source patterns: **/add-fw-test**. Compile only the animation +
`animation_registry.cpp` + `animation_base.cpp`. Every NEW animation needs a suite (SKILL.md step 9).

## [ ] 10. `fw/src/pattern_controller.cpp` — BOTH shell name spots
Anchor: `grep -n '"rainbow"\|(rainbow,' fw/src/pattern_controller.cpp` (two hits;
missing either silently breaks `anim set`/`anim get` — SKILL.md step 10)

- **Spot A — `anim set` dict**: `SHELL_SUBCMD_DICT_SET_CREATE(sub_anim_set, ...)`.
  Unconditional animations: add `(<name>, <id>, "<description>")` directly to the
  list. Kconfig-conditional: use the macro pattern — anchor
  `grep -n "MATRIX_CODE_SHELL_SUBCMD" fw/src/pattern_controller.cpp` — define
  `#if defined(CONFIG_ANIMATION_<NAME>) #define <NAME>_SHELL_SUBCMD , (<name>, <id>, "...") #else #define <NAME>_SHELL_SUBCMD #endif`
  and append the macro inside the dict like the existing three.
- **Spot B — `anim get` switch** in `cmd_anim_get()`: add
  `case Animation::<Name>: name = "<name>"; break;` (wrap in
  `#if defined(CONFIG_ANIMATION_<NAME>)` if conditional).

## [ ] 11. `.serial_mcp/plugins/rgb_sunglasses.py` — MCP name list (optional)
Anchor: `grep -n "SETTABLE_ANIMATIONS" .serial_mcp/plugins/rgb_sunglasses.py`

To make it drivable via `mcp__serial__rgb_sunglasses_set_animation`: add the shell name
(same string as step 10) to `SETTABLE_ANIMATIONS`; restart the serial MCP server.

## Validation ladder (run in this order)

1. **DI suite alone** (seconds):
   `twister -T fw/tests/animations/<name>_animation_di -p native_sim --outdir fw/twister-out-one`
   — the scratch `--outdir` is mandatory (/test-fw's targeted-run rule).
2. **Both boards link**: /build-proto0 then /build-dk; DK FLASH% overflow → gate the
   feature off the DK via board conf / Kconfig default (/rom-ram-budget). Confirm
   `CONFIG_ANIMATION_<NAME>=y` landed in `fw/build/fw/zephyr/include/generated/zephyr/autoconf.h`.
3. **Full gate**: /submit-pr (both builds + full test suite + coverage). On-device
   behavior is /flash-and-verify — hardware lock required, not covered here.
