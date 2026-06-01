# Animation Framework: Registry, Pattern Controller & Renderer Decoupling

## Context

Three concrete coupling points remain in the animation framework's base classes and infrastructure after the BT decoupling pass:

1. `BaseAnimationTemplate::setActive(bool)` calls `animation_registry_set_is_active(id, active)` directly — `src/animations/animation.h:28`
2. `AnimationIsActiveBinding<A>::onRemoteActiveChange(bool)` calls `pattern_controller_change_to_animation(id)` directly — `src/animations/animation_is_active_binding.h:62`
3. Every animation's `tick()` calls `pattern_controller_set_pixel_in_framebuffer(config, x, y, bufferId, r, g, b)` and reads `config->displayWidth` / `config->displayHeight` directly — all six animation `.cpp` files under `src/animations/` plus `src/animations/bt_animations.cpp`

All three are removed using abstract interface classes, consistent with the `AnimationUint32ParameterSource` / `FooAnimationDependencies` DI pattern.

---

## New abstract interfaces (all new files)

### `src/animations/animation_active_state_observer.h`
```cpp
#pragma once
#include <animations/animation_types.h>

class AnimationActiveStateObserver {
public:
    virtual ~AnimationActiveStateObserver() = default;
    virtual void onActiveStateChanged(Animation id, bool active) = 0;
};
```

### `src/animations/animation_activator.h`
```cpp
#pragma once
#include <animations/animation_types.h>

class AnimationActivator {
public:
    virtual ~AnimationActivator() = default;
    virtual void changeToAnimation(Animation id) = 0;
};
```

### `src/animations/animation_renderer.h`
```cpp
#pragma once
#include <cstddef>
#include <cstdint>

class AnimationRenderer {
public:
    virtual ~AnimationRenderer() = default;
    virtual size_t displayWidth() const = 0;
    virtual size_t displayHeight() const = 0;
    virtual void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) = 0;
};
```
This replaces the `const LedConfig*` + `bufferId` pair in `tick()`. Animations no longer include `led_config.h`.

---

## Per-file changes

### `src/animations/animation_base.h`

1. Change `tick()` signature:
   ```cpp
   virtual void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) = 0;
   ```
   Add `#include <animations/animation_renderer.h>`.

2. Add static observer slot:
   ```cpp
   static void registerActiveStateObserver(AnimationActiveStateObserver *observer);
   protected:
       static AnimationActiveStateObserver *sActiveStateObserver_;
   ```
   Add `#include <animations/animation_active_state_observer.h>`.

### `src/animations/animation_base.cpp` (new, tiny)

Defines the two statics and `registerActiveStateObserver`. Add to CMakeLists unconditionally.

### `src/animations/animation.h`

Change `setActive`:
```cpp
void setActive(bool active) override {
    if (sActiveStateObserver_) {
        sActiveStateObserver_->onActiveStateChanged(kAnimationId, active);
    }
}
```
Remove `#include <animations/animation_registry.h>` (no longer used here).

### `src/animations/animation_is_active_binding.h`

Add per-template-type activator (mirrors existing `setter_`):
```cpp
static void registerActivator(AnimationActivator *activator) {
    getInstance()->activator_ = activator;
}
static void onRemoteActiveChange(bool active) {
    if (active && getInstance()->activator_) {
        getInstance()->activator_->changeToAnimation(tAnimationId);
    }
}
AnimationActivator *activator_ = nullptr;
```
Replace `#include <pattern_controller.h>` with `#include <animations/animation_activator.h>`.

### `src/animations/{zigzag,rainbow,text,my_eyes}_animation.cpp`

Change `tick()` signature to `tick(AnimationRenderer &renderer, ...)`. Replace all:
- `config->displayWidth` → `renderer.displayWidth()`
- `config->displayHeight` → `renderer.displayHeight()`
- `pattern_controller_set_pixel_in_framebuffer(config, x, y, bufferId, r, g, b)` → `renderer.setPixel(x, y, r, g, b)`

Remove `#include <pattern_controller.h>` from each. `LedConfig *config` and `bufferId` parameters disappear from these TUs entirely.

### `src/animations/null_animation.cpp` and `src/animations/bt_animations.cpp`

Same tick signature change and pixel-writing updates.

### `src/animations/animation_registry_defaults.cpp`

Add concrete observer and activator implementations at file scope, and register them at the start of `animation_registry_register_defaults()`:

```cpp
class RegistryActiveStateObserver : public AnimationActiveStateObserver {
public:
    void onActiveStateChanged(Animation id, bool active) override {
        animation_registry_set_is_active(id, active);
    }
};

class PatternControllerActivator : public AnimationActivator {
public:
    void changeToAnimation(Animation id) override {
        pattern_controller_change_to_animation(id);
    }
};

static RegistryActiveStateObserver sRegistryObserver;
static PatternControllerActivator sActivator;
```

```cpp
// At top of animation_registry_register_defaults():
BaseAnimation::registerActiveStateObserver(&sRegistryObserver);
AnimationIsActiveBinding<Animation::Text>::registerActivator(&sActivator);
#if defined(CONFIG_ANIMATION_ZIGZAG)
AnimationIsActiveBinding<Animation::ZigZag>::registerActivator(&sActivator);
#endif
// ... Rainbow, MyEyes same pattern
```

Add `#include <pattern_controller.h>`, `#include <animations/animation_activator.h>`, `#include <animations/animation_active_state_observer.h>`.

### `src/pattern_controller.cpp`

Define concrete renderer and update tick call sites (lines 118 and 126):

```cpp
// Inside the render function, where LedConfig* and bufferId are local:
class PatternControllerRenderer : public AnimationRenderer {
    const LedConfig *config_;
    size_t bufferId_;
public:
    PatternControllerRenderer(const LedConfig *c, size_t b) : config_(c), bufferId_(b) {}
    size_t displayWidth() const override { return config_->displayWidth; }
    size_t displayHeight() const override { return config_->displayHeight; }
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
        pattern_controller_set_pixel_in_framebuffer(config_, x, y, bufferId_, r, g, b);
    }
};

// Replace:
//   anim->tick(get_current_led_config(), kTargetRenderIntervalMs, bufferId);
// With:
PatternControllerRenderer renderer(get_current_led_config(), bufferId);
anim->tick(renderer, kTargetRenderIntervalMs);
```

Add `#include <animations/animation_renderer.h>`.

### `tests/animations/*/src/main.cpp` (all four DI test harnesses)

Replace `animation->tick(&kTestConfig, timeSinceLastTickMs, bufferId)` with a `TestRenderer`:

```cpp
class TestRenderer : public AnimationRenderer {
public:
    size_t displayWidth() const override { return kTestConfig.displayWidth; }
    size_t displayHeight() const override { return kTestConfig.displayHeight; }
    void setPixel(size_t, size_t, uint8_t, uint8_t, uint8_t) override {}
};
TestRenderer renderer;
animation->tick(renderer, timeSinceLastTickMs);
```
Remove any `#include <pattern_controller.h>` from test files.

### `CMakeLists.txt`

Add `src/animations/animation_base.cpp` to the unconditional `target_sources` block.

---

## Files summary

| File | Change |
|---|---|
| `src/animations/animation_active_state_observer.h` | **New** — abstract interface |
| `src/animations/animation_activator.h` | **New** — abstract interface |
| `src/animations/animation_renderer.h` | **New** — abstract interface |
| `src/animations/animation_base.h` | New tick sig; add observer slot |
| `src/animations/animation_base.cpp` | **New** — defines statics + `registerActiveStateObserver` |
| `src/animations/animation.h` | `setActive` → observer; drop `animation_registry.h` |
| `src/animations/animation_is_active_binding.h` | Add `activator_`; `onRemoteActiveChange` → activator; drop `pattern_controller.h` |
| `src/animations/{zigzag,rainbow,text,my_eyes}_animation.cpp` | New tick sig; pixel writes → `renderer.setPixel`; geometry → `renderer.displayWidth/Height` |
| `src/animations/null_animation.cpp` | New tick sig |
| `src/animations/bt_animations.cpp` | New tick sig; pixel writes → renderer |
| `src/animations/animation_registry_defaults.cpp` | Concrete impls + registration; add `pattern_controller.h` |
| `src/pattern_controller.cpp` | `PatternControllerRenderer`; update tick call sites |
| `tests/animations/*/src/main.cpp` (×4) | Replace tick call with `TestRenderer` |
| `CMakeLists.txt` | Add `animation_base.cpp` |

---

## Success criteria

1. `west build` passes after each numbered implementation step.
2. `grep -rE 'animation_registry_set_is_active|pattern_controller' src/animations/animation.h src/animations/animation_is_active_binding.h` → zero matches.
3. `grep -rE 'pattern_controller_set_pixel_in_framebuffer|LedConfig' src/animations/{zigzag,rainbow,text,my_eyes,null}_animation.cpp` → zero matches.
4. `twister -T tests -p native_sim` — 6/6 pass.
5. On-device: all animations render correctly; `Is Active` notify works both directions.

---

## Implementation order

Each step is a buildable checkpoint. Do not proceed until `west build` passes.

1. Create three new abstract interface headers (`animation_renderer.h`, `animation_active_state_observer.h`, `animation_activator.h`). Build (no impact expected).
2. Add `animation_base.cpp`; update `animation_base.h` (observer slot + new tick sig); update `animation.h` (`setActive` → observer; drop registry include). Build.
3. Update `animation_is_active_binding.h` (activator field + `registerActivator`; drop `pattern_controller.h`). Build.
4. Update `src/pattern_controller.cpp` with `PatternControllerRenderer` and new tick call sites. Build.
5. Update all six animation `.cpp` files (new tick sig + `renderer.setPixel` + `renderer.displayWidth/Height`). Build.
6. Update `animation_registry_defaults.cpp` (concrete `RegistryActiveStateObserver` + `PatternControllerActivator`; registration calls). Build.
7. Update four DI test harnesses (`TestRenderer`; new tick call). Run `twister`. All 6/6 pass.
