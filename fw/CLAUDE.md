# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Agent behavior

- Always use built-in LLM tools to edit files
- Whenever I say "Remember that" or some similar equivalent, update this file with the information.

## Hardware Revisions

- The "rgb_sunglasses_dk" board is legacy. We need to retain build support for it, but no new features are added to this board.
- The "rgb_sunglasses_proto0" board is the latest hardware revision. Always enable new features on the proto0 hardware revision by default. When I ask you to add a new feature, ensure it's enabled on the Proto0 hardware KConfig

## Project vs SDK

- Files under the `fw` directory are ours to modify as we please.
- Files under the `~/ncs` directory are NOT modifyable. They are part of the SDK and can NEVER BE TOUCHED.

## Project applications

The firwmare is composed for 4 applications:

- MCUBoot: the appcore's bootloader
- rgb-sunglasses: the appcore main application
- b0n: the netcore bootloader
- ipc_radio: the netcore's main application

## Build and Test Commands

```bash
# First time build (pristine, setup build system, very slow! Only run if build folder is empty / nonexistent)
west build --build-dir /workspaces/rgb-sunglasses/fw/build /workspaces/rgb-sunglasses/fw --pristine --board rgb_sunglasses_proto0/nrf5340/cpuapp --sysbuild --cmake-only -- -DCONFIG_DEBUG_THREAD_INFO=y -DBOARD_ROOT="/workspaces/rgb-sunglasses/fw"

# Full incremental build (preferred)
west build --build-dir /workspaces/rgb-sunglasses/fw/build /workspaces/rgb-sunglasses/fw

# Run all tests on native simulator
twister -T /workspaces/rgb-sunglasses/fw/tests -p native_sim

# Run a single test suite
twister -T /workspaces/rgb-sunglasses/fw/tests/animations/animation_registry -p native_sim
```

Treat successful `west build` as the primary validation step after any change. The NCS SDK lives at `/root/ncs/v3.1.1`.

**Always use `west build` for building — never invoke `cmake` or `ninja` directly.** The `west build` command handles multi-image (sysbuild) coordination correctly; raw `cmake`/`ninja` invocations bypass that and produce misleading results.

Known non-blocking warning: `multi-line comment [-Wcomment]` in `src/bluetooth/bt_service.h`.

## Commenting rules

- **Preserve existing comments.** Never delete comments unless they are factually incorrect about the code that remains (e.g., a comment that describes a removed code path). Refactoring to change an API does not justify removing comments — update variable/function names in the comment text to match the new API, but keep the explanation.
- **Commented-out code (`/*...*/` or `//`) is intentional.** Developers in embedded projects often comment out alternative implementations, debug printk calls, or reference snippets as quick-enable stubs. Do not remove these blocks.
- **Add comments to non-obvious logic.** If you write code whose purpose or mechanism is not immediately clear from reading the code alone, add a comment.

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
  - **Requests a fast connection interval once security completes (issue #41)**: right before transitioning CONNECTING → CONNECTED, calls `bt_conn_le_param_update(ctx->conn, &fast_conn_param)` with `fast_conn_param = BT_LE_CONN_PARAM_INIT(6, 12, 0, 400)` (~7.5-15ms interval). Without this, the connection runs at whatever the central defaults to (Zephyr's own unrequested default is `BT_GAP_INIT_CONN_INT_MIN/MAX`, 30-50ms), and the app's discovery walk does ~170+ sequential GATT reads — one full connection interval each, since Android only allows one outstanding GATT op per connection. This is a belt-and-suspenders complement to the app's own `requestConnectionPriority(ConnectionPriority.High)` call in `use-ble-connection.ts` (see `app/CLAUDE.md`) — either side's request should produce the same effect; having both means it still works if one side's request doesn't take for some reason. A non-zero return from `bt_conn_le_param_update()` is logged but non-fatal.
  - **`bt_conn_info` shell command and `le_param_updated` connection callback (issue #41 follow-up)**: added to verify, rather than assume, what connection interval is actually in effect — `bt_conn_le_param_update()` only sends a _request_; it doesn't tell you what the central actually granted. `le_param_updated(conn, interval, latency, timeout)` (registered in `conn_callbacks`, a `BT_CONN_CB_DEFINE`) logs the real negotiated parameters every time they change, with a timestamp, so you can see exactly when/whether a fast-interval request converged relative to other events. `bt_conn_info` (a standalone `SHELL_CMD_REGISTER`, no subcommands) prints the _current_ parameters of `s_active_conn` (a diagnostic-only tracked pointer, ref-counted via the existing `connected()`/`disconnected()` callbacks) on demand — useful for polling mid-connection from a second shell session while the app is mid-discovery. Confirmed finding from using these: the interval briefly converges to 7.5ms right after `CONNECTED`, has a ~400ms excursion to 45ms about 2s in (during the app's `connectToDevice()`/GATT-cache-refresh phase, before the per-characteristic read loop starts), then settles at **15ms** (the slow end of our requested 7.5-15ms range) for the rest of the connection — not the 7.5ms fast end originally assumed. This is very likely Android's own `CONNECTION_PRIORITY_HIGH` policy floor (~11.25-15ms is its documented range), which as GAP central it has final say over regardless of what `fast_conn_param` proposes — there's no public Android API tier faster than "high priority" to request instead.
- `src/bluetooth/bt_service_cpp.h` — C++23 compile-time GATT server assembler. `BtGattServer<Providers...>` collects `BtGattAttributeProvider` objects, assigns auto UUIDs in provider-declaration order, and flattens them to a `bt_gatt_attr[]` backed by a `std::array`. Use `BT_GATT_SERVER_REGISTER(name, server)` to register with Zephyr.
  - **Bulk metadata characteristic, gated by `CONFIG_APP_BT_METADATA_CHARACTERISTIC` (issue #41 follow-up)**: `BtGattServer` automatically synthesizes and appends one extra read-only characteristic per service (fixed shared UUID `kMetadataCharacteristicUuid`, same pattern as `kAnimationNameCharacteristicUuid`) whose value is a compile-time-constant packed blob containing every sibling characteristic's CUD name + CPF format — `[version][entry_count]` then `[cpf_format][name_len][name_bytes]` per entry, in `Providers...` declaration order. This lets the app read one characteristic per service instead of two descriptor reads (CUD + CPF) per characteristic, cutting ATT op count roughly in half for discovery. **No per-service `.cpp` file needs to change** — the blob is derived automatically from the same `Providers...` pack each service already passes into `BtGattServer(...)`, via `getDescription()`/`getCpf()` static accessors added to `BtGattCharacteristicCommon` and the `BtGattMetadataBearingProvider` concept + `MetadataBlobBuilder<Ps...>` fold (both just above `BtGattServer` in this file), which skip the primary-service provider automatically since it doesn't expose those statics. Hardware-verified: discovery across all 9 services dropped from ~13-30s to ~6s with zero fallback/mismatch warnings.
    - **Disabled on `rgb_sunglasses_dk`** (`CONFIG_APP_BT_METADATA_CHARACTERISTIC=n` in its board `.conf`): the blob duplicates every characteristic's CUD description string as packed binary data, which pushed that board's image past its internal-flash slot size (confirmed: imgtool `Image size ... exceeds requested size` with this enabled) — same flash-budget reasoning as `CONFIG_APP_PERSIST_BT_CONFIG` below. When disabled, `getMetadataAttrsTuple()`/`kMetadataBlob` are never referenced and so are never instantiated (class template members are only instantiated when used) — zero flash cost on that board.
    - **Ordering assumption**: the app's positional zip (`use-ble-connection.ts`) assumes `characteristicsForService()` returns characteristics in the same order as this blob, i.e. firmware GATT declaration order. This holds because ATT "Read By Type" (used internally by characteristic discovery) is spec-required to return attributes in ascending handle order, and handles are assigned in exactly `Providers...`'s declaration order — not a platform convention that could silently change. Verified (this session) that react-native-ble-plx's Android module (`BleModule.java:511-517` → `Service.java:51-53`) passes Android's native `BluetoothGattService.getCharacteristics()` result straight through with no client-side re-sort. A same-count _reordering_ would not be caught by the blob's `entry_count` check — only a count mismatch is detected — this residual risk is accepted rather than paying for a fully self-describing (per-entry UUID-tagged) wire format.
  - Characteristic aliases: `BtGattReadWriteCharacteristic`, `BtGattReadNotifyCharacteristic`, `BtGattAutoReadWriteCharacteristic`, etc.
  - Write hooks: if a characteristic class defines `onWrite(const T&)`, it is called automatically after each successful remote write.
  - `src/bluetooth/persistent_characteristic.h` — `BtGattPersistentCharacteristic<Key, Description, Notify, T, Default>`: a `BtGattAutoCharacteristicExt` subclass (same shape as `IsActiveCharacteristic` below) that backs a plain POD/`BtGattColor`/`BtGattString<N>` characteristic with Zephyr's settings subsystem, so its value survives a power cycle. `Key` is an explicit string literal (e.g. `"core/brightness"`) — never derive it from declaration order, since `BtGattServer`'s auto-UUID assignment is positional but settings keys must stay stable across reorderings. See "Settings-backed config persistence" below for the full mechanism. `BtGattDropdownList<N>` characteristics (glim selection/loop mode) don't fit this generic mixin and persist by hand instead — see `glim_player_animation_bt.cpp`.
  - **`notify()` only sends the actual string length for string-backed types (`BtGattString<N>`/`BtGattDropdownList<N>`), matching `read()`'s `strnlen()`-based length** — not `sizeof(storage_)` (the full fixed-capacity buffer). A `bt_gatt_notify()` call cannot fragment a value across multiple ATT PDUs the way long writes/reads can; the whole payload must fit in one packet bounded by the connection's negotiated ATT MTU. Before this was fixed, a `BtGattDropdownList<512>` characteristic (e.g. `GlimSelectionCharacteristic`) always tried to notify the full 512-byte buffer regardless of how short the actual string content was, so every notify failed (`bt_att: No ATT channel for MTU ...` / `Notify failed: -12`) even with a 2-file (~28-byte) selection list — a real bug, not a hypothetical. On failure, `notify()` now logs the characteristic's `Description` and attempted payload length so an MTU-related failure can be traced to which characteristic caused it without guessing. The app side also needs an adequately large negotiated MTU in the first place — see `requestMTU` in `app/CLAUDE.md`'s Known Issues section.
- `src/animations/bt_animations.{h,cpp}` — BT-aware animation classes for the visual BT status indicators (advertising pulse, connecting flash, pairing code display). These are intentionally mixed-concern; they are the correct place to keep BT in animation code.
- `src/animations/animation_is_active_binding.h` — BT-free template that bridges the registry's `setActive` callback to a GATT characteristic setter; also routes remote BLE writes back to `pattern_controller_change_to_animation`.
- `src/animations/animation_is_active_characteristic.h` — `IsActiveCharacteristic<A>`: a `BtGattAutoCharacteristicExt` subclass that hooks `onWrite` to `AnimationIsActiveBinding<A>::onRemoteActiveChange`.

**DI interfaces (added in `animation-refactor-part2`)**

- `src/bluetooth/bt_state_observer.h` — `BtStateObserver`: pure abstract observer; `bluetooth.cpp` calls through this instead of including `pattern_controller.h` / `bt_animations.h`. Register with `bluetooth_register_state_observer()`.
- `src/configuration_provider.h` — `ConfigurationProvider`: abstract interface over `CoreConfig` singleton (getBrightnessFactor, getDisplayRateMs, getRenderRateMs). `CoreConfig` inherits from it. Injected into `led_controller` and `pattern_controller` via setter functions; lazy fallback to `CoreConfig::getInstance()` if not set.
- `src/button_event_listener.h` + `src/buttons.h` — `ButtonEventListener`: `onButtonPressed(size_t buttonId)`. Dispatch is ISR-safe: GPIO interrupt → `K_MSGQ_DEFINE` → `k_work` → listener on work-queue thread. Register with `buttons_register_listener()`. Button IDs 0–3 = sw0–sw3; ID 4 = wake button.
  - **Physical layout (proto0, a directional grid):** button 0 = Up, button 1 = Left, button 2 = Right, button 3 = Down. The devicetree labels in `boards/others/rgb_sunglasses_proto0/rgb_sunglasses_proto0_nrf5340_cpuapp_common.dts` reflect this (e.g. "Push button 1 (Up)"), but the `sw0`–`sw3` aliases themselves are unchanged. When wiring button behavior for a new animation, use this mapping rather than guessing — e.g. `GlimPlayerAnimation` (`src/animations/glim_player_animation.cpp`) uses button 0 (Up) to advance to the next GLIM file and button 3 (Down) to go to the previous one; buttons 1/2 (Left/Right) are intentionally unassigned there.

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
- `src/core_config.cpp` — device-level settings (brightness, display/render thread rates, status LED brightness), each backed by `BtGattPersistentCharacteristic` so they persist via Zephyr's settings subsystem (see below).

### Settings-backed config persistence

Every BT-settable config value (core config, animation parameters/strings/colors, glim selection/loop mode, and the currently-active animation) persists across power cycles via Zephyr's settings subsystem. The storage backend itself (`CONFIG_SETTINGS`/`CONFIG_SETTINGS_NVS`/`CONFIG_NVS`, the `settings_storage` NVS partition on external flash in `pm_static_rgb_sunglasses_proto0_nrf5340_cpuapp.yml`, and the `settings_load()` call in `bluetooth_init()`) predates this and exists for BT bonding — this just adds a second consumer.

- `src/settings/persistent_value_registry.{h,cpp}` — BT-free, fixed-size runtime registry (same shape as `animation_registry.cpp`) mapping a stable key string → `{target, load_fn, save_fn}`, self-populated by static-init constructors. Lets one shared settings subtree handler dispatch `settings_load()` callbacks to dozens of independently-registered values instead of needing one `SETTINGS_STATIC_HANDLER_DEFINE` per characteristic.
- `src/settings/persistent_value_store.{h,cpp}` — owns the single `SETTINGS_STATIC_HANDLER_DEFINE("appcfg", ...)` handler (forwards to the registry's dispatch) and a shared debounced `k_work_delayable`. `request_save()` (re)schedules a flush of every registered value `CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS` after the last call, coalescing rapid writes (typing a string, dragging a color picker) into one flash write. **Don't reuse `CONFIG_BT_SETTINGS_DELAYED_STORE_MS` for this or anything else BT-free** — this module intentionally has its own Kconfig symbol so it has no dependency on the Bluetooth stack.
- `src/bluetooth/persistent_characteristic.h` — see the GATT layer bullet above.
- Bespoke (non-mixin) persistence: `glim_player_animation_bt.cpp` persists the glim selection by **file name**, not index (`glim_registry`'s enumeration order can shift between boots) — and since `glim_registry::init()` runs after `settings_load()`, the loaded name is resolved to an index later, in `glim_player_animation_bind_default_bt_dependencies()`. `pattern_controller.cpp` persists a single "last active animation" key (hooked into `pattern_controller_change_to_animation()`) rather than per-animation booleans, to avoid reconstructing "which one was active" from independent flags.
- **`CONFIG_APP_PERSIST_BT_CONFIG`** (default `y`, `n` on `rgb_sunglasses_dk`) gates the whole feature: every call site above is wrapped in `if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG))` (in the template mixin) or `if (IS_ENABLED(...))` (in plain `.cpp` files), so the doLoad/doSave code is fully compiled out and linked away when disabled. This exists because DK's internal-flash image slot has no spare room for it (it was already at ~75% before this feature existed) and DK is legacy / doesn't get new features per the Hardware Revisions note above — disabling needed its own gate rather than `#ifdef`-ing every one of the ~33 characteristic declarations.

### Kconfig and optional features

Animations are conditionally compiled via:

```
CONFIG_ANIMATION_MY_EYES=y
CONFIG_ANIMATION_RAINBOW=y
CONFIG_ANIMATION_ZIGZAG=y
```

Text animation is always compiled. Audio is gated on `CONFIG_AUDIO`. Check `prj.conf` for the full configuration and memory-saving flags (`CONFIG_ASSERT=n`, `CONFIG_SIZE_OPTIMIZATIONS=y`).

**Don't reuse a Kconfig symbol from one subsystem to configure unrelated code in another, even if the value/semantics happen to line up.** E.g. a BT-free module's debounce/delay tunable should get its own `CONFIG_APP_*` symbol, not borrow `CONFIG_BT_SETTINGS_DELAYED_STORE_MS` just because the timing happens to match — that creates a hidden cross-subsystem dependency and works against this project's general push to decouple BT from non-BT code (see the animation/BT decoupling refactor above).

### SYS_INIT ordering for early registration

`SYS_INIT(fn, APPLICATION, N)` runs before `K_THREAD_DEFINE` threads are scheduled. Lower N runs first. When an observer or listener must be registered before a thread can fire its first event, use `SYS_INIT(APPLICATION, 0)`. Both `bluetooth_init` and `button_init` run at priority 1, so registering observers at priority 0 guarantees the observer is in place before either subsystem starts.

It's very important that SYS_INIT() priority levels must ALWAYS be a plain pre-processor directive that derives to a single number. SYS_INIT() priority levels CANNOT be expressions that require evaluation, they MUST be a plain number or a single pre-processor directive that is replaced directly with a number.

For example, this is illegal:

```
SYS_INIT(mcuboot_info_init, APPLICATION, CONFIG_RETENTION_BOOTLOADER_INFO_INIT_PRIORITY + 1);
```

This is also illegal:

```
#define MCUBOOT_INFO_INIT_PRIORITY (CONFIG_RETENTION_BOOTLOADER_INFO_INIT_PRIORITY + 1)
SYS_INIT(mcuboot_info_init, APPLICATION, MCUBOOT_INFO_INIT_PRIORITY);
```

To enforce init ordering, use a plain KConfig value and then add `static_assert()`s as needed to guarantee ordering.

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

Prefer changes under `/workspaces/rgb-sunglasses/fw` (app code). Only touch `/root/ncs/v3.1.1` (NCS SDK) when explicitly requested.

### Zephyr RTOS

This project uses the Zephyr RTOS.

Read the documentation directly from /root/ncs/v3.1.1/zephyr/doc

## Hardware Environment

Run `/check-hardware` at the start of any session to discover what's available. The skill checks lsusb for the dev board, verifies the TTY ports, and checks for a connected Android device via ADB.

## Serial Console (Zephyr Shell)

The dev board exposes two USB-CDC-ACM ports:

| Port           | Role                                        | Baud   |
| -------------- | ------------------------------------------- | ------ |
| `/dev/ttyACM0` | Zephyr interactive shell (`uart:~$` prompt) | 115200 |
| `/dev/ttyACM1` | MCUmgr UART transport (firmware updates)    | 115200 |

A SEGGER J-Link (VID:PID `1366:0101`) may also be connected for advanced operations (reflashing MCUboot, GDB debugging). `/check-hardware` probes it and prints status, VTref, and serial number. See [Flashing via J-Link](#flashing-via-j-link-fast-path) below for the fast flash path.

Note: `JLinkExe -CommandFile` only opens the USB connection lazily, on the first command that actually needs it — a command file containing just `Exit` never touches USB at all. The probe (and `jlink-flash.sh`) use `ShowHWStatus` to force the connect; that banner is also where the `S/N:` serial number comes from.

**If both ports are missing despite the board showing up in `lsusb`**, the `cdc_acm` kernel module is not loaded in the WSL docker-desktop VM. Run `wsl -d docker-desktop -- modprobe cdc_acm` from Windows, then replug the board.

- **Prompt**: `uart:~$` (appears after the boot log completes)

### Using the `mcp__serial__*` tools

**Always use the `mcp__serial__*` MCP tools to interact with the Zephyr shell.** Never shell out via Bash to read/write `/dev/ttyACM0` directly (e.g. `cat`/`echo` redirects, `screen`, `picocom`) — it races with the MCP server's background reader thread for ownership of the port and produces garbled/lost data.

**Graduate working shell interactions into serial MCP plugins.** Once you've figured out how to reliably drive a shell subsystem over raw `serial_write`/`serial_read_until` — correct command syntax, response parsing, any device-specific quirks — don't keep repeating that raw sequence in future sessions. Write or extend a plugin under `.serial_mcp/plugins/` (use `serial_plugin_template` to scaffold, `serial_plugin_load`/`serial_plugin_reload` to pick it up) so the next interaction is a single typed tool call instead of hand-rolled read/write. `rgb_sunglasses.py` (see below) is the first instance of this pattern, for the `anim` subsystem — add new plugin files (or new tools in the existing one) the same way for other shell subsystems as they come up.

**Wait for boot before sending commands.** Boot log output interleaves with shell echoed input and causes `command not found` errors. Wait until `uart:~$` appears before issuing any shell commands.

**Sending newlines correctly.** `serial_write` with `data: "\r\n"` sends the four literal characters `\`, `r`, `\`, `n` — not a CR+LF. Always use one of these instead:

```jsonc
// Option 1 — append_newline flag (preferred)
{ "data": "kernel version", "append_newline": true }

// Option 2 — explicit hex
{ "data": "kernel version\r", "as": "hex" }   // hex-encode the CR separately
```

### Animation shell control — the `rgb_sunglasses` serial MCP plugin

The `anim` shell command (`anim get` / `anim set <name>` / `anim indicator clear`,
defined in `src/pattern_controller.cpp`) is exposed as a serial MCP plugin at
`.serial_mcp/plugins/rgb_sunglasses.py` — prefer it over hand-rolled
`serial_write`/`serial_read_until` calls, which are error-prone (see the prompt
redraw quirk below). Requires `SERIAL_MCP_PLUGINS=rgb_sunglasses` in `.mcp.json`'s
`serial` server env (already set); reconnect via `/mcp` after enabling. As other
shell subsystems get plugin coverage, add their tools to this same file (or a
new file under `.serial_mcp/plugins/`) and update `SERIAL_MCP_PLUGINS` accordingly.

Tools: `rgb_sunglasses.get_animation`, `rgb_sunglasses.set_animation` (name one of
`none, zigzag, text, rainbow, my_eyes, beat, fft_bars, bad_apple, nyan_cat`),
`rgb_sunglasses.clear_indicator`.

**Always clear the active BT indicator before starting an animation.** A BT
indicator (advertising/connecting/pairing overlay) overrides whatever animation
is set and will visually hide it. `rgb_sunglasses.set_animation` already does
this automatically (calls `clear_indicator` before `anim set` and verifies via
`anim get`) — don't bypass it by calling the shell directly.

**Zephyr shell prompt redraw quirk:** the shell redraws `uart:~$` after _every_
async log line (BT notifications, GLIM decoder logs, etc.), not just after a
command finishes. A naive `read_until("uart:~$ ")` can match a stale redraw left
over from a previous command's delayed logging, before the current command's
own echo has even arrived — this caused a real false-failure during testing.
The plugin works around it by flushing the input buffer before each write, then
accumulating `read_until` chunks until the command's own echo is found followed
by a prompt (see `_run_command` in the plugin file).

**Stray input right after a board reset:** a boot-log fragment (e.g. `rf: Preinit`)
can land in the shell's own input line editor before the first command is ever
sent, corrupting it (observed as `command not found` on the very first call after
reset). `_run_command` sends Ctrl+C before every command to cancel whatever's
sitting in the line editor, not just on the first call — cheap and fully general.

**Trying out new GLIM content:** `GlimPlayerAnimation` (`anim set glim_player`) replaced the
old per-file `bad_apple`/`nyan_cat` animations — it enumerates every `.glim` file under
`/NAND:/glim` on boot (`glim_registry`) and can play any of them, picked via BLE (a
generic "drop-down list" characteristic, see `glim_player_animation_bt.cpp`), the
`glim` shell command (`glim list` / `glim select <index>` / `glim get_selected` /
`glim set_loop_mode <mode>`), or a button press (sw0 cycles to the next file). It reads
geometry/frame-count/pixel-format (mono `Raw` or `Rgb24`) from each file's own header
rather than hardcoding anything, so trying new footage is just a matter of dropping a
new `.glim` file into `/NAND:/glim/` and resetting — no firmware rebuild needed; see
`tools/convert_video_to_glim.py` / `tools/convert_bad_apple.py` / `tools/convert_gif_to_glim.py`
to generate one.

**Setting up GLIM files on a new board:** All GLIM assets are generated using in-repo Python scripts — nothing is checked in as a binary. Generate them before provisioning:

```bash
python3 fw/tools/generate_nyan_cat_glim.py --output fw/nyan_cat.glim
python3 fw/tools/convert_bad_apple.py --output fw/bad_apple.glim   # downloads from YouTube, ~1 min
```

Then copy both into `/NAND:/glim/` on the board and reset. If the NAND disk on a new board is unformatted (FAT read errors in dmesg), it needs `mkfs.vfat -F 12 -s 8 -S 4096 /dev/sdX` before mounting — ask the user to run this as it is destructive.

### Useful shell commands

```
kernel version          # print Zephyr/NCS version
kernel threads          # list all threads and their stack usage
bt connect              # (if shell BT commands are enabled)
bt_conn_info             # print the *actual* current LE connection interval/latency/timeout
                         # (see bluetooth.cpp's le_param_updated callback for the issue #41
                         # connection-interval investigation this was added for)
```

## USB Flash Disk (`/NAND:` — GLIM/animation assets)

The dev board exposes a 4 MiB FAT filesystem over USB Mass Storage (SCSI Bulk-Only,
interface 4 of the composite USB device). This is the "NAND" disk Zephyr mounts at
`/NAND:` (`src/storage/storage.cpp`; LUN registered in `src/usb/usb_init.c` as
`USBD_DEFINE_MSC_LUN(nand, "NAND", "RGB-SG", "FlashDisk", "0.00")`). It's how
`bad_apple.glim`, `nyan_cat.glim`, and similar assets get onto the device (see
`fw/tools/convert_bad_apple.py`, `generate_nyan_cat_glim.py`).

**Finding and mounting it from the devcontainer:**

```bash
# It enumerates as a SCSI disk alongside the container's own disks — identify it
# by the SCSI string, not a fixed /dev/sdX (the letter shifts based on what else
# is attached).
dmesg | grep -A2 "RGB-SG"       # confirms detection, e.g. "scsi 1:0:0:0: Direct-Access RGB-SG FlashDisk"
lsblk                            # cross-reference the ~4 MiB size to find the device node, e.g. /dev/sdg
blkid /dev/sdg                   # TYPE="vfat" confirms it's the right one

mkdir -p /mnt/sunglasses-fs
mount -o rw /dev/sdg /mnt/sunglasses-fs
cp bad_apple.glim nyan_cat.glim /mnt/sunglasses-fs/
sync
umount /mnt/sunglasses-fs
rmdir /mnt/sunglasses-fs
```

**The board will not see new/changed files until it's reset.** After unmounting,
reset via mcumgr (`mcumgr --conntype serial --connstring dev=/dev/ttyACM1,baud=115200 reset`)
or a physical reset — the firmware's own FAT mount has to be re-established before
`bad_apple`/`nyan_cat` (or anything else that opens `/NAND:`) can see newly-copied
files. Wait ~15s for `ttyACM*` to re-enumerate, then re-run `/check-hardware` before
issuing more serial commands.

## Flashing via J-Link (fast path)

When `/check-hardware` reports the J-Link `Status: OK`, prefer flashing over it instead of the slow MCUmgr/UART upload path below.

```bash
fw/scripts/jlink-flash.sh                  # uses fw/build by default
fw/scripts/jlink-flash.sh /path/to/build    # explicit build dir
fw/scripts/jlink-flash.sh -- --skip-rebuild # extra args forwarded to `west flash`
```

`jlink-flash.sh` auto-detects the attached J-Link's serial number and runs `west flash -d <build-dir> --dev-id <serial>` — no need to hardcode or look up `--dev-id` yourself. `/check-hardware` also prints the serial directly under the J-Link section (`Serial: ...`) if you need it for some other tool.

- This triggers a `west build` rebuild-check first (fast no-op if nothing changed), then flashes via the **`nrfutil` runner** (not raw `JLinkExe`) — it programs both `merged_CPUNET.hex` (netcore) and `merged.hex` (appcore), each with erase → program → verify → reset.
- Typical total time: ~30-45s, plus ~15s for USB re-enumeration afterward. Re-run `/check-hardware` to confirm both ttyACM ports are back before issuing further serial/mcumgr commands.
- This is the only way to reflash the bootloader (MCUboot/b0n); MCUmgr can only update the application images.

## MCUmgr

`mcumgr` is installed in the devcontainer (built from source during image build). The MCUmgr port is always USB interface x.2 — run `/check-hardware` to find the current port (it shifts after resets; see WSL2 note below).

```bash
# Run /check-hardware first to identify the current MCUmgr port (may be ttyACM1, ttyACM2, etc.)
CONN="--conntype serial --connstring dev=/dev/ttyACM2,baud=115200"  # example — verify with /check-hardware

mcumgr $CONN image list       # list firmware images
mcumgr $CONN echo "hello"     # connectivity check
mcumgr $CONN reset            # soft-reset the device
```

### Image layout

The board exposes two images via MCUmgr (confirmed from `image list`):

| image | Slot | Core                      |
| ----- | ---- | ------------------------- |
| 0     | 0    | App core (rgb-sunglasses) |
| 1     | 0    | Net core (ipc_radio)      |

Both currently report `version: 0.0.0` — the build version string is not yet wired up.

### Firmware update flow

```bash
CONN="--conntype serial --connstring dev=/dev/ttyACM1,baud=115200"

# 1. Upload the signed image (~678 KiB at ~3.5 KiB/s over serial = ~3-4 minutes, be patient)
mcumgr $CONN image upload fw/build/fw/zephyr/zephyr.signed.bin

# 2. Get the hash of the uploaded image (slot 1 of image 0)
mcumgr $CONN image list

# 3. Mark it for test boot
mcumgr $CONN image test <hash>

# 4. Reset — MCUboot will boot the new image
mcumgr $CONN reset

# 5. Wait ~15 seconds for the board to re-enumerate on USB before issuing further commands
#    /dev/ttyACM* disappears during reset and takes noticeably longer than expected to return.
```

After a successful test boot, run `image confirm` (or let the app confirm via BLE) to make it permanent. If the device fails to boot, MCUboot reverts to the previous image automatically.

### Commands

```bash
mcumgr $CONN taskstat      # list all threads with stack/runtime info
mcumgr $CONN stat list     # list stat groups (e.g. flash_sim_stats)
mcumgr $CONN stat read flash_sim_stats
```

- `taskstat` requires `CONFIG_THREAD_MONITOR=y`, `CONFIG_MCUMGR_GRP_OS_TASKSTAT=y`, and a large-enough TX FIFO on the CDC-ACM mcumgr port (see DTS note below). All three are set on proto0.
- `shell exec` — returns status=8 (ENOTSUP); the Zephyr shell is on ACM0, not the MCUmgr transport

### CDC-ACM TX FIFO (why `hw-flow-control` matters)

The `zephyr,cdc-acm-uart` driver's `poll_out` silently **drops bytes** when the TX ring buffer is full and `hw-flow-control` is NOT set. With the default 1024-byte FIFO a multi-frame `taskstat` response (~1850 wire bytes) overflows mid-stream and the client times out.

Fix applied in `rgb_sunglasses_proto0_nrf5340_cpuapp_common.dts`:

```dts
cdc_acm_uart1: cdc_acm_uart1 {
    compatible = "zephyr,cdc-acm-uart";
    hw-flow-control;      /* poll_out blocks instead of dropping */
    tx-fifo-size = <4096>;
};
```

`hw-flow-control` makes `poll_out` sleep 1 ms and retry when the buffer is full. `tx-fifo-size = 4096` is large enough to hold a full taskstat response without blocking at all.

### WSL2 / udev: ttyACM node numbering can shift

After a firmware reset, the board re-enumerates as a new USB device. The Linux kernel assigns the next available ACM minor numbers — if the previous ttyACM0 node wasn't cleaned up, the new device gets ttyACM1/ttyACM2 instead. Additionally, WSL2's udev sometimes fails to create /dev nodes for new ACM interfaces even though they appear in sysfs.

**If mcumgr times out after a reset:**

```bash
# Check sysfs for all registered ACM devices
ls /sys/class/tty/ttyACM*

# Create any missing /dev nodes
for d in /sys/class/tty/ttyACM*; do
    n=$(basename $d)
    maj_min=$(cat $d/dev)
    maj=${maj_min%:*}; min=${maj_min#*:}
    [ -e /dev/$n ] || mknod /dev/$n c $maj $min && chmod 666 /dev/$n
done

# Determine which port is mcumgr by trying each one
for p in /dev/ttyACM*; do
    echo -n "$p: "
    mcumgr --conntype serial --connstring dev=$p,baud=115200 echo ping 2>&1 | head -1
done
```

The mcumgr port is whichever responds to `echo`. Update `CONN` accordingly.

**This also breaks already-open `mcp__serial__*` connections, not just mcumgr.** After flashing via J-Link (`jlink-flash.sh` resets the board) or any other board reset, an existing `mcp__serial__*` connection_id to the Zephyr shell goes stale: the first write after the reset fails with an I/O error (`[Errno 5] Input/output error`), and `serial_open` on the _same path_ then fails with `[Errno 6] No such device or address` because the board re-enumerated under a new ttyACM minor number (the old path's underlying device is just gone). Fix: `serial_close` the stale connection_id, re-run `/check-hardware` (or the `ls`/`mknod` loop above) to find the shell's _new_ port, then `serial_open` on that new path. Don't retry the old connection_id or the old path — it will keep failing.

## Build Failures

If a build fails, prefer to read the log files instead of building it again.

## Per-image Kconfig/devicetree overlays (sysbuild)

This is a sysbuild project with 4 images sharing one board-level devicetree. To scope a change to a single image (e.g. MCUboot only), use sysbuild's per-image config directory convention, not `fw/conf/<board>/sysbuild.cmake`:

```
fw/sysbuild/<image-name>/prj.conf                          # per-image Kconfig fragment
fw/sysbuild/<image-name>/boards/<board>.conf                # per-image, per-board Kconfig fragment
fw/sysbuild/<image-name>/boards/<board>.overlay              # per-image, per-board devicetree overlay
```

e.g. `fw/sysbuild/mcuboot/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.overlay` only applies to the MCUboot image. These are auto-discovered by Zephyr's CMake — no wiring needed beyond creating the file in the right place.

**`add_overlay_dts(${DEFAULT_IMAGE}, ...)` in `fw/conf/<board>/sysbuild.cmake` targets the main "fw" app image, not MCUboot.** `${DEFAULT_IMAGE}` is sysbuild's default/main image, which is `fw` in this project. Don't reach for this mechanism when you actually want to target MCUboot, b0n, or ipc_radio — use the per-image `sysbuild/<image-name>/` directory above instead.

**A newly-added overlay file may not be picked up without a `--pristine` rebuild.** Zephyr's overlay auto-discovery (`zephyr_file(CONF_FILES ... DTC_OVERLAY_FILE ...)`) is gated behind `if(NOT DEFINED DTC_OVERLAY_FILE)` — if a prior configure already ran and cached `DTC_OVERLAY_FILE` (even as an empty string) in `build/<image>/CMakeCache.txt`, the auto-discovery block is permanently skipped on every subsequent incremental build, even after adding the right file in the right place. If a new overlay doesn't seem to take effect, check `grep DTC_OVERLAY_FILE build/<image>/CMakeCache.txt` — if it's defined-but-empty, do a full `--pristine` rebuild instead of debugging the overlay content.

## MCUboot and LED data pins (GPIO retention across warm resets)

MCUboot never links the SPI/LED_STRIP drivers, so the 3 WS2812 data-in pins (P0.29, P1.05, P1.01 — see `fw/docs/proto0-board-pinout.md`) are left completely unmanaged during MCUboot's runtime. On the nRF53, GPIO peripheral state (direction/level) is retained across a CPU/software (warm) reset — only a power-on/brownout reset clears it. So if the app was driving a data line high before a warm reset (e.g. the `sys_reboot(SYS_REBOOT_WARM)` that follows MCUmgr's `image test`/`reset`), MCUboot inherits that stuck-high state, and a WS2812 strip can read it as a steady "on" signal and pull max-brightness current for the entire bootloader boot window — risking a brownout that prevents boot entirely.

Fixed via Zephyr's built-in GPIO hogs feature (`CONFIG_GPIO_HOGS`, auto-enabled by the presence of `gpio-hog` devicetree nodes): see `fw/sysbuild/mcuboot/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.overlay`. It forces all 3 pins to a driven-low GPIO output very early in boot (`SYS_INIT` priority 41), independent of any driver, for MCUboot's entire runtime. If you add more LED data pins in future hardware revisions, extend this overlay too.
