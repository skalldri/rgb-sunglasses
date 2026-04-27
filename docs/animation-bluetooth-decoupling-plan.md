# Animation / Bluetooth Decoupling Refactor Plan

## Objective

Finish the physical separation of animation logic from Bluetooth/GATT code on the `animation-refactor` branch. After this pass, every translation unit under `src/animations/*_animation.cpp` compiles **without** any Bluetooth header on its include path, and the existing dependency-injection test scaffolds in `tests/animations/*_di/` become fully standalone (no `CONFIG_BT=y` required in their `prj.conf`).

Secondary objective: reduce the "default dependencies" boilerplate a little while we're in the neighborhood — remove the nullable `deps_` pointer check in `tick()` and friends, since `animation_registry_register_defaults()` always binds dependencies before any tick.

## Context / current state

The refactor has already introduced the right abstractions:

- [src/animations/animation_parameter_source.h](../src/animations/animation_parameter_source.h) — abstract `AnimationUint32ParameterSource` with a single `get()` method
- [src/animations/text_animation.h](../src/animations/text_animation.h) — abstract `TextAnimationSlotSource` / `TextAnimationUpNextSource`
- [src/animations/my_eyes_animation.h](../src/animations/my_eyes_animation.h) — abstract `MyEyesAnimationSlotSource` / `MyEyesAnimationUpNextSource`
- Per-animation `FooAnimationDependencies` structs holding `const` refs
- Per-animation `setDependencies()` method + `sDefaultDeps` module-static + free `foo_animation_bind_default_dependencies()` helper
- [src/animations/animation_is_active_binding.h](../src/animations/animation_is_active_binding.h) — abstract, no BT
- [src/animations/animation_registry.{h,cpp}](../src/animations/animation_registry.cpp) — BT-free

What remains wrong: every `*_animation.cpp` still **physically** contains:

1. `#include <bluetooth/bt_service_cpp.h>` and `#include <zephyr/bluetooth/uuid.h>`
2. `BtGattPrimaryService<...>` and `BtGattAutoReadWriteCharacteristic<...>` instances at file scope
3. Concrete `ParameterSource` subclasses that **read those GATT characteristics directly** to implement `get()`
4. Module-static `sDefault*Source` / `sDefaultDeps` instances
5. An `IsActiveCharacteristic<...>` instance and its `BT_GATT_SERVER_REGISTER` macro
6. An `IsActiveBindingRegistrar` static struct that wires `AnimationIsActiveBinding::registerSetter(...)` into a GATT setter
7. The `foo_animation_bind_default_dependencies()` function itself

And `animation.h` still transitively leaks BT via two vestigial includes:

```cpp
// src/animations/animation.h lines 5-6
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/gatt_cpf.h>
```

These were needed when `BaseAnimationTemplate` inherited BT base classes. After earlier phases of the refactor it no longer does, so they are dead weight that poisons every `*_animation.h` that includes `animation.h`.

Finally, two animations have **hidden coupling in their constructors**:

- `TextAnimation::TextAnimation()` calls `setTextSlot(i, kStaticMessages[i])` — writes directly into the `textSlot0..19` GATT characteristics ([text_animation.cpp:310-318](../src/animations/text_animation.cpp#L310-L318)).
- `MyEyesAnimation::MyEyesAnimation()` does the same via `setMyEyesSlot(i, kStaticEyes[i])` ([my_eyes_animation.cpp:315-323](../src/animations/my_eyes_animation.cpp#L315-L323)).

This means it's not enough to relocate the BT code out of the bottom of each `.cpp` — the constructor's default-content initialization needs to move to the adapter too.

There is also a small block of **WIP scratch BT code** at [text_animation.cpp:14-23](../src/animations/text_animation.cpp#L14-L23) declaring a `kMyServiceUuid` / `characteristicA` / `serverStatic`. `TextUpNextSource::consumeCurrentAndAdvance` writes to `characteristicA` ([text_animation.cpp:293](../src/animations/text_animation.cpp#L293)). Confirm intended behavior before moving or deleting.

## User design choices (already decided)

The following were decided at planning time and should guide implementation:

1. **Keep Meyer's singleton** (`BaseAnimationTemplate::getInstance()`) and `setDependencies()` — do NOT switch to constructor injection in this pass. Smaller diff; constructor injection is follow-up work.
2. **Adapter files live under `src/bluetooth/animation_adapters/`** — not inlined into `src/animations/` with a `bt_` prefix.
3. **Is-active coupling is in scope.** Move `IsActiveCharacteristic`, the registrar, and the local-to-GATT setter to the adapter files; also move `animation_is_active_characteristic.h` out of `src/animations/` into `src/bluetooth/`.
4. **No new unit test target in this pass.** The existing [tests/animations/*_di/](../tests/animations) scaffolds already exist; they become usable after the refactor. Extending coverage is follow-up work.

## Success criteria

1. `west build` passes on the primary board target.
2. On-device verification: each animation is selectable from the companion app; each parameter (step time, color, width, up-next, slots) is mutable from the app and reflected in the animation; the `Is Active` characteristic notifies changes correctly in both directions.
3. `twister -T tests -p native_sim` passes.
4. Every `tests/animations/*_di/prj.conf` has `CONFIG_BT=y` and `CONFIG_BT_PERIPHERAL=y` **removed** and still passes.
5. Scan confirms no Bluetooth includes remain under `src/animations/`:

   ```
   grep -rE 'bluetooth|BT_GATT|BtGatt' src/animations/
   ```

   Expect zero matches (other than the intentional `bt_animations.cpp` / `bt_animations.h` pair, which remains the animation-enumeration integration point).

## Target file layout

```text
src/animations/
    animation_base.h                         (unchanged, pure)
    animation.h                              (BT includes REMOVED)
    animation_parameter_source.h             (unchanged)
    animation_types.h                        (unchanged)
    animation_registry.{h,cpp}               (unchanged)
    animation_is_active_binding.h            (unchanged, abstract)
    animation_registry_defaults.cpp          (unchanged)
    null_animation.{h,cpp}                   (unchanged — already no BT)
    zigzag_animation.{h,cpp}                 (.cpp stripped of BT)
    rainbow_animation.{h,cpp}                (.cpp stripped of BT)
    text_animation.{h,cpp}                   (.cpp stripped of BT; ctor no longer touches slots)
    my_eyes_animation.{h,cpp}                (.cpp stripped of BT; ctor no longer touches slots)
    bt_animations.{h,cpp}                    (unchanged)

src/bluetooth/
    bt_gatt_traits.h                         (unchanged)
    bt_service_cpp.h                         (unchanged)
    gatt_cpf.h                               (unchanged)
    animation_is_active_characteristic.h     (MOVED from src/animations/)
    animation_adapters/                      (NEW directory)
        zigzag_animation_bt.cpp              (NEW)
        rainbow_animation_bt.cpp             (NEW)
        text_animation_bt.cpp                (NEW)
        my_eyes_animation_bt.cpp             (NEW)
```

## Per-file changes

### 1. [src/animations/animation.h](../src/animations/animation.h) — drop vestigial BT includes

Delete lines 5-6:

```cpp
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/gatt_cpf.h>
```

Verify the remaining file still compiles by itself: `BaseAnimationTemplate` only references `BaseAnimation`, `Animation` (enum), `animation_registry_set_is_active`, and `size_t`. All those come from the remaining `animation_base.h`, `animation_types.h`, `animation_registry.h`, `pattern_controller.h`, `zephyr/kernel.h` includes.

### 2. [src/animations/animation_is_active_characteristic.h](../src/animations/animation_is_active_characteristic.h) → `src/bluetooth/animation_is_active_characteristic.h`

Move the file. It includes `<bluetooth/bt_service_cpp.h>` and defines a `BtGattAutoCharacteristicExt` subclass — it is BT. Update the include path in each consumer (currently the four `*_animation.cpp` files; after the refactor, only the four adapter `.cpp` files).

No content changes needed other than the file location.

### 3. ZigZag split — simplest case, do this first

**[src/animations/zigzag_animation.cpp](../src/animations/zigzag_animation.cpp)** becomes (whole file):

```cpp
#include <animations/zigzag_animation.h>

#include <zephyr/sys/__assert.h>

void ZigZagAnimation::setDependencies(const ZigZagAnimationDependencies &deps)
{
    deps_ = &deps;
}

void ZigZagAnimation::init()
{
    currentCycleTimeMs = 0;
    currentIndex = 0;
}

void ZigZagAnimation::tick(const LedConfig *config, const size_t timeSinceLastTickMs, const size_t bufferId)
{
    __ASSERT(deps_, "ZigZagAnimation::tick before setDependencies");

    currentCycleTimeMs += timeSinceLastTickMs;

    if (currentCycleTimeMs > deps_->stepTimeMs.get())
    {
        currentCycleTimeMs = 0;
        currentIndex++;
    }

    if (currentIndex >= (config->displayWidth * config->displayHeight))
    {
        currentIndex = 0;
    }

    for (size_t x = 0; x < config->displayWidth; x++)
    {
        for (size_t y = 0; y < config->displayHeight; y++)
        {
            pattern_controller_set_pixel_in_framebuffer(config, x, y, bufferId, 0, 0, 0);
        }
    }

    size_t y = currentIndex / config->displayWidth;
    size_t x = currentIndex % config->displayWidth;

    uint32_t color = deps_->color.get();
    uint8_t red = (color >> 16) & 0xFF;
    uint8_t green = (color >> 8) & 0xFF;
    uint8_t blue = (color >> 0) & 0xFF;
    pattern_controller_set_pixel_in_framebuffer(config, x, y, bufferId, red, green, blue);
}
```

Note: the `if (!deps_) setDependencies(sDefaultDeps);` fallback is removed (see "The `deps_` fallback" section below).

**New [src/bluetooth/animation_adapters/zigzag_animation_bt.cpp](../src/bluetooth/animation_adapters/zigzag_animation_bt.cpp)** (receives everything currently at [zigzag_animation.cpp:1-71](../src/animations/zigzag_animation.cpp#L1-L71) except the class methods):

```cpp
#include <animations/zigzag_animation.h>
#include <animations/animation_is_active_binding.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>

#include <zephyr/bluetooth/uuid.h>

constexpr bt_uuid_128 kZigZagConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x0200, 0x56789abd0000));

BtGattPrimaryService<kZigZagConfigServiceUuid> zigzagPrimaryService;
BtGattAutoReadWriteCharacteristic<"Step Time Ms", uint32_t, 200> zigzagStepTimeMs;
BtGattAutoReadWriteCharacteristic<"Color", BtGattColor, BtGattColor{0xFFFFFFFF}> zigzagColor;

using ZigZagIsActiveCharacteristic = IsActiveCharacteristic<Animation::ZigZag>;
ZigZagIsActiveCharacteristic zigzagIsActive;

BtGattServer zigzagConfigServer(
    zigzagPrimaryService,
    zigzagStepTimeMs,
    zigzagColor,
    zigzagIsActive);
BT_GATT_SERVER_REGISTER(zigzagConfigServerStatic, zigzagConfigServer);

namespace
{
    class ZigZagStepTimeSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override { return zigzagStepTimeMs; }
    };

    class ZigZagColorSource : public AnimationUint32ParameterSource
    {
    public:
        uint32_t get() const override { return static_cast<BtGattColor>(zigzagColor); }
    };

    ZigZagStepTimeSource sDefaultStepTimeSource;
    ZigZagColorSource sDefaultColorSource;
    ZigZagAnimationDependencies sDefaultDeps(sDefaultStepTimeSource, sDefaultColorSource);
}

using ZigZagAnimationIsActive = AnimationIsActiveBinding<Animation::ZigZag>;

static void zigzag_set_is_active(bool active)
{
    zigzagIsActive.setActive(active);
}

struct ZigZagIsActiveBindingRegistrar
{
    ZigZagIsActiveBindingRegistrar()
    {
        ZigZagAnimationIsActive::registerSetter(zigzag_set_is_active);
    }
};

[[maybe_unused]] ZigZagIsActiveBindingRegistrar sZigZagIsActiveBindingRegistrar;

void zigzag_animation_bind_default_dependencies()
{
    ZigZagAnimation::getInstance()->setDependencies(sDefaultDeps);
}
```

The forward declaration `void zigzag_animation_bind_default_dependencies();` stays in [zigzag_animation.h](../src/animations/zigzag_animation.h) (already present, BT-free). [animation_registry_defaults.cpp](../src/animations/animation_registry_defaults.cpp) still calls it — no change there.

### 4. Rainbow split

Shape is identical to ZigZag. Source file to lift from: [rainbow_animation.cpp:1-74](../src/animations/rainbow_animation.cpp#L1-L74).

Retain in `src/animations/rainbow_animation.cpp`: the `rainbowColors[]` palette, `rainbowBrightness`, `RainbowAnimation::setDependencies`, `init`, and `tick`. Remove the `if (!deps_) setDependencies(sDefaultDeps);` fallback at [rainbow_animation.cpp:111-114](../src/animations/rainbow_animation.cpp#L111-L114) and replace with an `__ASSERT(deps_, ...)`.

Move to `src/bluetooth/animation_adapters/rainbow_animation_bt.cpp`: the service UUID, `rainbowStepTimeMs`, `rainbowWidthPix`, `rainbowIsActive`, `rainbowConfigServer`, `BT_GATT_SERVER_REGISTER`, the two source subclasses, `sDefault*` statics, the is-active registrar, and `rainbow_animation_bind_default_dependencies()`.

### 5. Text split — most involved

**[src/animations/text_animation.cpp](../src/animations/text_animation.cpp) becomes:**

- Retain: `TextAnimation::setDependencies`, `TextAnimation::init`, `TextAnimation::tick`, `TextAnimation::getStringFromSlot`, `TextAnimation::getUpNext`, `LOG_MODULE_REGISTER(text_anim, LOG_LEVEL_INF)`. (The `FontAtlas` include stays too — font rendering is not BT.)
- **The `TextAnimation::TextAnimation()` constructor must stop populating GATT slots.** Change the body to be empty (or delete the constructor — class can use the default since there's no longer any initialization work beyond the member initializers):

  ```cpp
  TextAnimation::TextAnimation() = default;
  ```

  Also consider removing the constructor declaration from [text_animation.h:46](../src/animations/text_animation.h#L46) if there's no longer any implementation file work — defaulting it inline in the header is equivalent.
- Delete: all BT headers, `kMyServiceUuid`/`primaryService`/`characteristicA`/`server`/`serverStatic` (WIP scratch), the text-config service UUID + primary, all `textStepTimeMs`/`textColor`/`textUpNext`/`textSlot0..19`, `textIsActive`, `textConfigServer`, `BT_GATT_SERVER_REGISTER(textConfigServerStatic, ...)`, the `TextStepTimeSource`/`TextColorSource`/`TextSlotSource`/`TextUpNextSource` subclasses, all `sDefault*` instances, the is-active registrar, `kStaticMessages`, `getTextSlot`/`setTextSlot`, `kNumStringSlots`, and `text_animation_bind_default_dependencies()`.
- Delete the three `if (!deps_) setDependencies(sDefaultTextDeps);` fallbacks at [text_animation.cpp:332-335](../src/animations/text_animation.cpp#L332-L335), [342-345](../src/animations/text_animation.cpp#L342-L345), [359-362](../src/animations/text_animation.cpp#L359-L362) and replace with an `__ASSERT` at the top of each.

**New `src/bluetooth/animation_adapters/text_animation_bt.cpp` contains:**

- All 20 `textSlotN` characteristics and all other GATT declarations currently at [text_animation.cpp:14-84](../src/animations/text_animation.cpp#L14-L84).
- `kNumStringSlots`, `kStaticMessages`, the `getTextSlot`/`setTextSlot` helpers, the four source subclasses, `sDefault*` statics, `sDefaultTextDeps`, the is-active `using` + setter + registrar, and `text_animation_bind_default_dependencies()`.
- The WIP `kMyServiceUuid` / `primaryService` / `characteristicA` / `server` / `serverStatic` block also moves here. (Verify with user whether this should be kept or deleted — `TextUpNextSource::consumeCurrentAndAdvance` writes to `characteristicA`, so removing it requires also removing that write.)
- **Initial slot population.** Move the loop from the deleted `TextAnimation::TextAnimation()` constructor into an init-time adapter construct. Simplest form — a static struct whose constructor runs at file-scope init:

  ```cpp
  namespace
  {
      struct TextSlotInitializer
      {
          TextSlotInitializer()
          {
              for (size_t i = 0; i < kNumStringSlots; i++)
              {
                  setTextSlot(i, kStaticMessages[i]);
              }
          }
      };

      [[maybe_unused]] TextSlotInitializer sTextSlotInitializer;
  }
  ```

  This runs during C++ static initialization — after all the slot GATT characteristics exist as globals but before `main()`. `animation_registry_register_defaults()` is called from `main()`, so by the time `text_animation_bind_default_dependencies()` runs, the slots already contain `kStaticMessages`.

  Static-init ordering note: `setTextSlot` mutates the `BtGattString` storage of the `textSlotN` instances; those instances must be fully constructed before `sTextSlotInitializer`'s constructor runs. Declaring the slots *above* `sTextSlotInitializer` in the same TU guarantees this under C++'s in-order static init rules for a single TU.

### 6. MyEyes split

Shape is identical to Text. Source: [my_eyes_animation.cpp:1-328](../src/animations/my_eyes_animation.cpp#L1-L328).

Retain in `src/animations/my_eyes_animation.cpp`: `MyEyesAnimation::setDependencies`, `init`, `tick`, `getStringFromSlot`, `getUpNext`, `LOG_MODULE_REGISTER`. Constructor becomes `= default`. Remove the four `if (!deps_) setDependencies(sDefaultMyEyesDeps);` fallbacks at [my_eyes_animation.cpp:297-300](../src/animations/my_eyes_animation.cpp#L297-L300), [307-310](../src/animations/my_eyes_animation.cpp#L307-L310), [342-345](../src/animations/my_eyes_animation.cpp#L342-L345), and the in-constructor call to `setDependencies(sDefaultMyEyesDeps)` at [my_eyes_animation.cpp:322](../src/animations/my_eyes_animation.cpp#L322). Replace the fallbacks with `__ASSERT` at each call site.

Move to `src/bluetooth/animation_adapters/my_eyes_animation_bt.cpp`: service UUID, all 20 `myEyesSlotN` characteristics, `myEyesBlinkSpeedMs`, `myEyesColor`, `myEyesUpNext`, `myEyesIsActive`, `myEyesConfigServer`, `BT_GATT_SERVER_REGISTER`, `kNumStringSlots`, `kStaticEyes`, `getMyEyesSlot`/`setMyEyesSlot`, the four source subclasses, `sDefault*` statics, `sDefaultMyEyesDeps`, the is-active registrar, and `my_eyes_animation_bind_default_dependencies()`.

Add a `MyEyesSlotInitializer` struct mirroring `TextSlotInitializer` above.

### 7. [src/animations/animation_registry_defaults.cpp](../src/animations/animation_registry_defaults.cpp) — no changes

This file already uses only BT-free APIs (`AnimationIsActiveBinding<>::setLocalActiveState`, factory functions, and the `*_bind_default_dependencies()` forward declarations from each animation's public header). Its calls to `text_animation_bind_default_dependencies()` etc. still link — the definitions simply live in the adapter TU now.

### 8. Top-level [CMakeLists.txt](../CMakeLists.txt) — add adapter sources

Add after the existing `target_sources_ifdef` block at [CMakeLists.txt:35-38](../CMakeLists.txt#L35-L38):

```cmake
target_sources(app PRIVATE src/bluetooth/animation_adapters/text_animation_bt.cpp)

target_sources_ifdef(CONFIG_ANIMATION_MY_EYES app PRIVATE src/bluetooth/animation_adapters/my_eyes_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_RAINBOW app PRIVATE src/bluetooth/animation_adapters/rainbow_animation_bt.cpp)
target_sources_ifdef(CONFIG_ANIMATION_ZIGZAG  app PRIVATE src/bluetooth/animation_adapters/zigzag_animation_bt.cpp)
```

The Text adapter is unconditional, matching the current treatment of [text_animation.cpp at CMakeLists.txt:27](../CMakeLists.txt#L27).

These could be gated on `CONFIG_BT` if a no-BT firmware build is a goal — but current firmware always links BT, so mirroring the existing `CONFIG_ANIMATION_*` gates is sufficient.

### 9. Test `prj.conf` cleanups

For each of:

- [tests/animations/zigzag_animation_di/prj.conf](../tests/animations/zigzag_animation_di/prj.conf)
- [tests/animations/rainbow_animation_di/prj.conf](../tests/animations/rainbow_animation_di/prj.conf)
- [tests/animations/text_animation_di/prj.conf](../tests/animations/text_animation_di/prj.conf)
- [tests/animations/my_eyes_animation_di/prj.conf](../tests/animations/my_eyes_animation_di/prj.conf)

Delete the lines:

```text
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y
```

No CMakeLists.txt changes needed — each test already compiles only the pure animation `.cpp` + `animation_registry.cpp` (see e.g. [tests/animations/zigzag_animation_di/CMakeLists.txt](../tests/animations/zigzag_animation_di/CMakeLists.txt)).

The [tests/animations/animation_registry/](../tests/animations/animation_registry/) test is already independent of the above and should remain untouched.

## The `deps_` fallback in `tick()` and friends

Every animation currently starts its `tick()` (and for Text/MyEyes, also `getStringFromSlot` / `getUpNext`) with:

```cpp
if (!deps_) { setDependencies(sDefaultDeps); }
```

After the split, `sDefaultDeps` lives in the adapter TU and is unreachable from the pure animation TU. The fallback could be preserved via a weakly-linked forward declaration of `foo_animation_bind_default_dependencies()`, but this adds mechanism for marginal value.

**Recommendation:** delete the fallback and replace with

```cpp
__ASSERT(deps_, "FooAnimation::tick called before setDependencies");
```

Rationale:

- `animation_registry_register_defaults()` runs from `main()` before the pattern controller starts ticking, and binds every animation.
- Tests always bind explicitly in their setup (see [tests/animations/zigzag_animation_di/src/main.cpp:89-95](../tests/animations/zigzag_animation_di/src/main.cpp#L89-L95)).
- An assert fails loud and cheap in debug builds; it compiles away in release.
- Keeping a fallback to a per-animation default in the pure TU would require cross-TU linking back to the adapter, partially defeating the separation.

## Implementation order

Each step below is a buildable checkpoint. Do not proceed to the next step until `west build` passes.

1. **Remove BT includes from [animation.h](../src/animations/animation.h).** Build. (Expected to still pass: each `*_animation.cpp` currently includes BT headers directly, so removing the transitive pull from `animation.h` doesn't break those TUs.)

2. **Relocate `animation_is_active_characteristic.h`** from `src/animations/` to `src/bluetooth/`. Update its include path in the four `*_animation.cpp` files (it will later only be included from the adapter files). Build.

3. **Split ZigZag first** (smallest, simplest). Create `src/bluetooth/animation_adapters/zigzag_animation_bt.cpp`, move code into it, strip `zigzag_animation.cpp`, drop the `deps_` fallback in `tick()`, update [CMakeLists.txt](../CMakeLists.txt) to compile the new adapter. Build. Flash and verify on hardware: the ZigZag animation still runs, its step-time and color are still writable from the companion app, and its Is Active characteristic still toggles correctly in both directions.

4. **Split Rainbow.** Same shape as ZigZag. Build + on-device verify.

5. **Split Text.** More work because of the slot-init move and the WIP scratch code. First confirm with user whether to keep/delete the `kMyServiceUuid`/`characteristicA`/`server` block. Then:
   - Create `src/bluetooth/animation_adapters/text_animation_bt.cpp`
   - Move all BT code + slot helpers + default content + `TextSlotInitializer`
   - Strip `text_animation.cpp`; make `TextAnimation::TextAnimation() = default;`
   - Remove all three `deps_` fallbacks; add asserts
   - Update [CMakeLists.txt](../CMakeLists.txt)
   - Build + on-device verify: text scrolls, slot writes from the companion app take effect, the Up Next notify fires.

6. **Split MyEyes.** Same shape as Text. Build + on-device verify.

7. **Strip `CONFIG_BT=y` / `CONFIG_BT_PERIPHERAL=y`** from the four DI-test `prj.conf` files. Run `twister -T tests -p native_sim`. Expect all tests to pass without BT compiled in.

8. **Final sweep.** Run:

   ```
   grep -rE 'bluetooth|BT_GATT|BtGatt' src/animations/
   ```

   Expect zero matches (the intentional BT integration point is at `src/animations/bt_animations.{h,cpp}`, which should still be BT-aware — confirm those are the only hits if any surface).

## Critical files (summary)

Files to edit:

- [src/animations/animation.h](../src/animations/animation.h) — delete lines 5-6
- [src/animations/zigzag_animation.cpp](../src/animations/zigzag_animation.cpp) — strip BT
- [src/animations/rainbow_animation.cpp](../src/animations/rainbow_animation.cpp) — strip BT
- [src/animations/text_animation.cpp](../src/animations/text_animation.cpp) — strip BT; default-ctor; remove slot init
- [src/animations/my_eyes_animation.cpp](../src/animations/my_eyes_animation.cpp) — strip BT; default-ctor; remove slot init
- [CMakeLists.txt](../CMakeLists.txt) — add adapter sources
- [tests/animations/zigzag_animation_di/prj.conf](../tests/animations/zigzag_animation_di/prj.conf) — drop `CONFIG_BT*`
- [tests/animations/rainbow_animation_di/prj.conf](../tests/animations/rainbow_animation_di/prj.conf) — drop `CONFIG_BT*`
- [tests/animations/text_animation_di/prj.conf](../tests/animations/text_animation_di/prj.conf) — drop `CONFIG_BT*`
- [tests/animations/my_eyes_animation_di/prj.conf](../tests/animations/my_eyes_animation_di/prj.conf) — drop `CONFIG_BT*`

Files to move:

- `src/animations/animation_is_active_characteristic.h` → `src/bluetooth/animation_is_active_characteristic.h`

Files to create:

- `src/bluetooth/animation_adapters/zigzag_animation_bt.cpp`
- `src/bluetooth/animation_adapters/rainbow_animation_bt.cpp`
- `src/bluetooth/animation_adapters/text_animation_bt.cpp`
- `src/bluetooth/animation_adapters/my_eyes_animation_bt.cpp`

## Risk list and mitigations

1. **Static-init ordering: slot GATT chars must be constructed before `TextSlotInitializer` / `MyEyesSlotInitializer` runs.**
   - Mitigation: within a single TU, C++ guarantees static objects are initialized in declaration order. Declaring `textSlotN` / `myEyesSlotN` above the initializer struct suffices. Do NOT split slot declarations across multiple TUs.

2. **Linker can't find `*_bind_default_dependencies()` when `CONFIG_ANIMATION_*=n`.**
   - [animation_registry_defaults.cpp](../src/animations/animation_registry_defaults.cpp) already guards calls to these with `#if defined(CONFIG_ANIMATION_ZIGZAG)` etc. Keep those guards; the adapter sources are also gated on the same configs in CMakeLists, so the functions are available exactly when the registry calls them.
   - Text is unconditional on both sides — keep it that way.

3. **Initial slot population differs from pre-refactor.**
   - Before: slots populated by `TextAnimation` constructor (runs on first `getInstance()` call, which is during `animation_registry_register_defaults()`).
   - After: slots populated by `TextSlotInitializer` static constructor (runs earlier, during C++ static init before `main()`).
   - Behavioral effect: slots are populated earlier, not later. Unless something else in the app queries slot contents during static init (no evidence of this), behavior is unchanged from a user-visible standpoint.

4. **WIP scratch code in [text_animation.cpp:14-23](../src/animations/text_animation.cpp#L14-L23).**
   - The `kMyServiceUuid` / `characteristicA` / `server` block is referenced by `TextUpNextSource::consumeCurrentAndAdvance` ([text_animation.cpp:293](../src/animations/text_animation.cpp#L293)). Recent commit `229c365 WIP Sound debugging` suggests this is transitional.
   - Mitigation: preserve the block as-is in the text adapter during this refactor; treat its disposition as out of scope. Flag to user before deleting.

5. **Assert-after-unbound becomes a crash where before it self-recovered.**
   - If anything did call `tick()` before `animation_registry_register_defaults()` ran, the old code would silently bind. Under the refactor, that becomes an `__ASSERT` failure.
   - Mitigation: the registration runs very early in `main()` before the pattern controller. On-device smoke test catches any path we missed.

6. **`animation_is_active_characteristic.h` move breaks other includers.**
   - Grep before moving: `grep -r 'animation_is_active_characteristic.h' src/`. Current hits are only the four `*_animation.cpp` files. Updating those four (before they're further edited) is safe.

## What's explicitly NOT in this pass (follow-up work)

- Constructor injection / dropping `BaseAnimationTemplate::getInstance()`. User indicated preference for this eventually, but is accepting this smaller-diff relocation first.
- Generalizing `AnimationUint32ParameterSource` to additional primitive types (bool, float, RGB) or a template-based `AnimationParameterSource<T>`.
- Runtime parameter discovery / metadata-driven GATT service assembly (letting the companion app introspect parameters from the device).
- Adding additional unit tests. The existing DI scaffolds become usable after this refactor; extending their coverage is follow-up.
- Deleting or formalizing the WIP `kMyServiceUuid` / `characteristicA` scratch in text_animation. Currently transitional code related to sound debugging per the recent commit.
- Splitting `bt_animations.{h,cpp}` itself; it is the intentional BT integration point for animation enumeration and remains mixed-concern by design.
