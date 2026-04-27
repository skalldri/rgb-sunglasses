# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and Test Commands

```bash
# First time build (pristine, setup build system, very slow! Only run if build folder is empty / nonexistent)
west build --build-dir /workspaces/rgb-sunglasses/build /workspaces/rgb-sunglasses --pristine --board rgb_sunglasses_dk/nrf5340/cpuapp --sysbuild --cmake-only -- -DCONFIG_DEBUG_THREAD_INFO=y -DBOARD_ROOT="/workspaces/rgb-sunglasses"

# Full incremental build (preferred)
west build --build-dir /workspaces/rgb-sunglasses/build /workspaces/rgb-sunglasses

# Run all tests on native simulator
twister -T /workspaces/rgb-sunglasses/tests -p native_sim

# Run a single test suite
twister -T /workspaces/rgb-sunglasses/tests/animations/animation_registry -p native_sim
```

Treat successful `west build` as the primary validation step after any change. The NCS SDK lives at `/root/ncs/v3.1.1`.

Known non-blocking warning: `multi-line comment [-Wcomment]` in `src/bluetooth/bt_service.h`.

## Architecture Overview

This is a Zephyr RTOS / Nordic Connect SDK (NCS) firmware project for RGB LED sunglasses. The target SoC is an nRF53 series device. The codebase is mixed C/C++; `main.c` is C but most application logic is C++23.

### Subsystems and their roles

**LED rendering pipeline**
- `src/led_controller.cpp` — manages dual-bank WS2812 LED strip hardware and a double-framebuffer. Callers claim a buffer via `claimBufferForRender`, write pixels via `set_pixel_in_framebuffer`, then release it.
- `src/pattern_controller.cpp` — sits above the LED controller. Owns the active animation slot and an optional `Indicator` overlay (BT advertising/connecting/pairing). Callers request an indicator with `pattern_controller_request_indicator` or switch animations with `pattern_controller_change_to_animation`.
- `src/led_config.h` — compile-time constants for the frame LED geometry (40×12 logical display over two banks, serpentine wiring) and the devkit LED geometry (8×2). All rendering code receives a `const LedConfig*` so the same logic runs on both targets.

**Animation system**
- `src/animations/animation_base.h` — pure abstract `BaseAnimation` with `init()`, `tick()`, and `setActive()`.
- `src/animations/animation.h` — `BaseAnimationTemplate<T, A>` CRTP base that adds a Meyer's singleton (`getInstance()`) and wires `setActive()` to the registry.
- `src/animations/animation_types.h` — `Animation` enum (ZigZag, Text, Rainbow, BtAdvertising, etc.).
- `src/animations/animation_registry.{h,cpp}` — runtime map of `Animation` → factory function + optional is-active setter callback. BT-free. Populated by `animation_registry_register_defaults()`.
- `src/animations/animation_registry_defaults.cpp` — registers all animations and calls each animation's `bind_default_dependencies()` helper; conditionally compiled via `CONFIG_ANIMATION_*` Kconfig symbols.
- Each animation (`zigzag`, `rainbow`, `text`, `my_eyes`) has a dependency struct holding `const` references to `AnimationUint32ParameterSource` (or similar abstract interfaces). The animation's `tick()` reads parameters only through these interfaces, keeping animation logic BT-free.

**Bluetooth / GATT layer**
- `src/bluetooth.cpp` — BT thread + state machine (IDLE → ADVERTISING → CONNECTING → CONNECTED). Uses `K_MSGQ` to decouple connection callbacks from state transitions. Requires `BT_SECURITY_L4` before transitioning to CONNECTED.
- `src/bluetooth/bt_service_cpp.h` — C++23 compile-time GATT server assembler. `BtGattServer<Providers...>` collects `BtGattAttributeProvider` objects, assigns auto UUIDs in provider-declaration order, and flattens them to a `bt_gatt_attr[]` backed by a `std::array`. Use `BT_GATT_SERVER_REGISTER(name, server)` to register with Zephyr.
  - Characteristic aliases: `BtGattReadWriteCharacteristic`, `BtGattReadNotifyCharacteristic`, `BtGattAutoReadWriteCharacteristic`, etc.
  - Write hooks: if a characteristic class defines `onWrite(const T&)`, it is called automatically after each successful remote write.
- `src/animations/bt_animations.{h,cpp}` — BT-aware animation classes for the visual BT status indicators (advertising pulse, connecting flash, pairing code display). These are intentionally mixed-concern; they are the correct place to keep BT in animation code.
- `src/animations/animation_is_active_binding.h` — BT-free template that bridges the registry's `setActive` callback to a GATT characteristic setter; also routes remote BLE writes back to `pattern_controller_change_to_animation`.
- `src/animations/animation_is_active_characteristic.h` — `IsActiveCharacteristic<A>`: a `BtGattAutoCharacteristicExt` subclass that hooks `onWrite` to `AnimationIsActiveBinding<A>::onRemoteActiveChange`.

**DI interfaces (added in `animation-refactor-part2`)**
- `src/bluetooth/bt_state_observer.h` — `BtStateObserver`: pure abstract observer; `bluetooth.cpp` calls through this instead of including `pattern_controller.h` / `bt_animations.h`. Register with `bluetooth_register_state_observer()`.
- `src/configuration_provider.h` — `ConfigurationProvider`: abstract interface over `CoreConfig` singleton (getBrightnessFactor, getDisplayRateMs, getRenderRateMs). `CoreConfig` inherits from it. Injected into `led_controller` and `pattern_controller` via setter functions; lazy fallback to `CoreConfig::getInstance()` if not set.
- `src/button_event_listener.h` + `src/buttons.h` — `ButtonEventListener`: `onButtonPressed(size_t buttonId)`. Dispatch is ISR-safe: GPIO interrupt → `K_MSGQ_DEFINE` → `k_work` → listener on work-queue thread. Register with `buttons_register_listener()`. Button IDs 0–3 = sw0–sw3; ID 4 = wake button.

**Important: `CoreConfig` getters are non-const.** `getBrightnessFactor()` writes back to clamp the value against the BT characteristic range. Any abstract interface it implements must therefore declare those methods without `const`, otherwise `CoreConfig` becomes abstract and `Singleton<CoreConfig>` fails to instantiate.

### Active refactor: animation / BT decoupling (`animation-refactor-part2` branch)

The goal is to remove all BT headers from `src/animations/*_animation.cpp`. Each animation's GATT service, parameter sources, and is-active wiring are being moved to new adapter files in `src/bluetooth/animation_adapters/`. See `docs/animation-bluetooth-decoupling-plan.md` for the full plan, per-file changes, and implementation order.

The refactor is in progress. When editing animation code, check whether the animation has already been split (adapter file exists under `src/bluetooth/animation_adapters/`) or still holds BT code in its `.cpp`.

After the refactor, verify the separation with:
```bash
grep -rE 'bluetooth|BT_GATT|BtGatt' src/animations/
# Expect zero matches outside bt_animations.{h,cpp}
```

### Other subsystems

- `src/power.cpp` / `drivers/` — TPS25750 USB PD controller (custom driver, patch loaded via LZ4-compressed blob) and BQ25792 battery charger (custom driver). I2C-based.
- `src/buttons.cpp` — GPIO button handling. Button callback runs in ISR context; dispatch to `ButtonEventListener` is deferred via `K_MSGQ_DEFINE` + `k_work` for thread safety.
- `src/fonts/` — `FontAtlas` and `FontShell` provide bitmap font rendering used by `TextAnimation` and `BtPairingAnimation`.
- `src/sound/sound.cpp` — PDM microphone via VM3011 driver; conditionally compiled with `CONFIG_AUDIO`.
- `src/core_config.cpp` — device-level settings persisted via Zephyr's settings subsystem.

### Kconfig and optional features

Animations are conditionally compiled via:
```
CONFIG_ANIMATION_MY_EYES=y
CONFIG_ANIMATION_RAINBOW=y
CONFIG_ANIMATION_ZIGZAG=y
```
Text animation is always compiled. Audio is gated on `CONFIG_AUDIO`. Check `prj.conf` for the full configuration and memory-saving flags (`CONFIG_ASSERT=n`, `CONFIG_SIZE_OPTIMIZATIONS=y`).

### SYS_INIT ordering for early registration

`SYS_INIT(fn, APPLICATION, N)` runs before `K_THREAD_DEFINE` threads are scheduled. Lower N runs first. When an observer or listener must be registered before a thread can fire its first event, use `SYS_INIT(APPLICATION, 0)`. Both `bluetooth_init` and `button_init` run at priority 1, so registering observers at priority 0 guarantees the observer is in place before either subsystem starts.

### Test structure

Tests live under `tests/` as Zephyr Twister test suites using `ztest`. Each suite has its own `CMakeLists.txt`, `prj.conf`, and `testcase.yaml`:
- `tests/animations/animation_registry/` — unit tests for the registry itself.
- `tests/animations/*_animation_di/` — dependency-injection tests for each animation, compiling the pure animation `.cpp` without BT. These currently require `CONFIG_BT=y` in their `prj.conf` only because animation `.cpp` files still include BT headers; removing BT from the animations is the goal of the active refactor.
- `tests/bt_state_observer/` — interface contract tests for `BtStateObserver` (does not link `bluetooth.cpp`).
- `tests/configuration_provider/` — interface contract tests for `ConfigurationProvider`.
- `tests/power/tps25750_patch_decompression/` — verifies the LZ4-compressed TPS25750 patch round-trips correctly.

**Twister `testcase.yaml` naming**: The `name` field must use a dotted `category.name` format (e.g., `interfaces.bt_state_observer`). A plain single-word name causes a `TwisterException` at runtime.

**C++23 in test `prj.conf`**: Use `CONFIG_STD_CPP2B=y` (not `CONFIG_STD_CPP23` — that symbol does not exist). Also add `CONFIG_REQUIRES_FULL_LIBCPP=y` and `CONFIG_REQUIRES_FULL_LIBC=y`.

**Test isolation from heavy dependencies**: If a registration function (e.g., `bluetooth_register_state_observer`) lives in a file with heavy BT stack dependencies, avoid linking that file in unit tests. Test the interface/observer contract directly on a mock implementation without calling the real registration function.

### Scope reminder

Prefer changes under `/workspaces/rgb-sunglasses` (app code). Only touch `/root/ncs/v3.1.1` (NCS SDK) when explicitly requested.

### Zephyr RTOS
This project uses the Zephyr RTOS.

Read the documentation directly from /root/ncs/v3.1.1/zephyr/doc
