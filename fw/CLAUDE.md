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

## Build directories — never mix boards in the same dir

| Board | Build dir | When to use |
|---|---|---|
| `rgb_sunglasses_proto0` | `fw/build` | Day-to-day development (incremental) |
| `rgb_sunglasses_dk` | `fw/build-dk` | Pre-PR validation only |

Switching boards inside the same build dir forces a full pristine rebuild (minutes of wasted time). Always use the correct dir for each board.

Use the project skills — `/build-proto0`, `/build-dk`, `/test-fw` — instead of raw `west build` commands. Use `/submit-pr` instead of manually pushing and creating PRs; it enforces both-board builds and coverage gates.

For which skill fits which task (built-in animation vs. `.llext` extension vs. GATT characteristic, debugging, flashing, releases), see the **Task routing** table in the root `CLAUDE.md` — it is the single routing table; do not duplicate it here.

**Before any `git push` or PR creation**, you must:
1. Run `/build-proto0` — proto0 must compile clean
2. Run `/build-dk` — DK must compile clean (no flash overflow)
3. Run `/test-fw` — all tests must pass. (`/test-fw` reports **overall** coverage only; the ≥ 50% **patch**-coverage gate is checked by `/submit-pr`.)

## Build and Test Commands (raw — prefer the skills above)

```bash
# First time build (pristine, setup build system, very slow! Only run if build folder is empty / nonexistent)
# Exception: a newly ADDED devicetree overlay file also requires --pristine — see "Per-image Kconfig/devicetree overlays (sysbuild)" below.
west build --build-dir fw/build fw --pristine --board rgb_sunglasses_proto0/nrf5340/cpuapp --sysbuild --cmake-only -- -DCONFIG_DEBUG_THREAD_INFO=y -DBOARD_ROOT="$(pwd)/fw"

# Full incremental build of proto0 (preferred for daily dev)
west build --build-dir fw/build fw --board rgb_sunglasses_proto0/nrf5340/cpuapp --sysbuild -- -DBOARD_ROOT="$(pwd)/fw"

# DK build (pre-PR validation)
west build --build-dir fw/build-dk fw --board rgb_sunglasses_dk/nrf5340/cpuapp --sysbuild -- -DBOARD_ROOT="$(pwd)/fw"

# Run all tests on native simulator
twister -T fw/tests -p native_sim

# Run a single test suite
twister -T fw/tests/animations/animation_registry -p native_sim
```

Commands above are relative to the repo root (or worktree root) — always run them from there, not from inside `fw/`.

Treat successful `west build` as the primary validation step after any change. The NCS SDK lives at `/root/ncs/v3.1.1`.

**Always use `west build` for building — never invoke `cmake` or `ninja` directly.** The `west build` command handles multi-image (sysbuild) coordination correctly; raw `cmake`/`ninja` invocations bypass that and produce misleading results.

Known non-blocking warning: `multi-line comment [-Wcomment]` in `src/bluetooth/bt_service.h`.

## Commenting rules

- **Preserve existing comments.** Never delete comments unless they are factually incorrect about the code that remains (e.g., a comment that describes a removed code path). Refactoring to change an API does not justify removing comments — update variable/function names in the comment text to match the new API, but keep the explanation.
- **Commented-out code (`/*...*/` or `//`) is intentional.** Developers in embedded projects often comment out alternative implementations, debug printk calls, or reference snippets as quick-enable stubs. Do not remove these blocks.
- **Add comments to non-obvious logic.** If you write code whose purpose or mechanism is not immediately clear from reading the code alone, add a comment.
- **Never put a `/*` sequence inside a comment** — glob paths like `/NAND:/ext/*.llext` trip `-Wcomment` ("/* within comment"). Rephrase as ".llext files in /NAND:/ext".

## Coding rules

- **Always use bounded string copies** (`strncpy` + explicit NUL, `snprintf`, `memcpy` with a checked length) — never `strcpy`/`sprintf`, even when the buffers are provably the same size today (PR #89 review feedback).
- **Never do flash/filesystem I/O from a cooperative-priority thread** — a long flash write starves every other thread in the system. Do it from a low-priority workqueue instead (PR #51).
- **Wrap every multi-step I2C/register transaction in a per-device `k_mutex`** (e.g. the TPS25750 I2Cm bridge's CMD1/DATA1 sequences), with `_locked` inner functions so every early return releases the lock, and bounded poll loops (timeout → `-ETIMEDOUT`) instead of infinite ones. Interleaving corruption shows up as **plausible-but-wrong values** (e.g. VBAT read back as the VBUS value), not as I2C errors (PR #111).
- **No info-level logs in steady-state/per-tick paths** (render ticks, notify calls, poll loops) — they become permanent log spam that buries real events (PR #110).

This is a Zephyr RTOS / Nordic Connect SDK (NCS) firmware project for RGB LED sunglasses. The target SoC is an nRF53 series device. The codebase is mixed C/C++; `main.c` is C but most application logic is C++23.

### Subsystems and their roles

**LED rendering pipeline**

- `src/led_controller.cpp` — manages dual-bank WS2812 LED strip hardware and a double-framebuffer. Callers claim a buffer via `claimBufferForRender`, write pixels via `set_pixel_in_framebuffer`, then release it.
- `src/pattern_controller.cpp` — sits above the LED controller. Owns the active animation slot and an optional `Indicator` overlay (BT advertising/connecting/pairing). Callers request an indicator with `pattern_controller_request_indicator` or switch animations with `pattern_controller_change_to_animation`. **Note `pattern_controller_change_to_animation()` runs synchronously on the caller's thread** — BT RX for GATT writes, the shell thread for `anim set`; only the boot-time restore in the thread entry runs on the pattern-controller thread itself. Nothing in this file may assume pattern-controller-thread context, and no automated gate checks this.
- `src/led_config.h` — compile-time constants for the frame LED geometry (40×12 logical display over two banks, serpentine wiring) and the devkit LED geometry (8×2). All rendering code receives a `const LedConfig*` so the same logic runs on both targets.

**Animation system**

- `src/animations/animation_base.h` — pure abstract `BaseAnimation` with `init()`, `tick()`, and `setActive()`.
- `src/animations/animation.h` — `BaseAnimationTemplate<T, A>` CRTP base that adds a Meyer's singleton (`getInstance()`) and wires `setActive()` to the registry.
- `src/animations/animation_types.h` — `Animation` enum (ZigZag, Text, Rainbow, BtAdvertising, etc.).
- `src/animations/animation_registry.{h,cpp}` — runtime map of `Animation` → factory function + optional is-active setter callback. BT-free. Populated by `animation_registry_register_defaults()`. **Registration order matters and returns must be checked**: `animation_registry_register_is_active()` returns `-ENOENT` unless `animation_registry_register()` already created the id's entry — an ignored return here silently killed the extensions' entire Is Active read/notify path on PR #89 (invisible to every build/test/shell gate; only a real app connection exposed it).
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
  - **Refusing a GATT write: return an ATT error, never "success + corrective notify"** (hardware-verified on PR #89). The app applies its optimistic update when the write *response* arrives, and a notification sent from inside the write handler reaches the phone *before* that response — so the optimistic update lands last and clobbers the corrective value; the UI shows the write as accepted. Returning `BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED)` instead makes the app's own catch-and-revert restore the previous value deterministically (see `write_is_active` in `src/extensions/extension_bt.cpp`). Notifications are the right tool only for state changes that originate device-side (e.g. a sandbox fault long after the write completed).
- `src/animations/bt_animations.{h,cpp}` — animation classes for the visual BT status indicators (advertising pulse, connecting flash, pairing code display). BT-themed by design, but driven externally (via `BtStateObserver`) — see "Animation / BT decoupling" below.
- `src/animations/animation_is_active_binding.h` — BT-free template that bridges the registry's `setActive` callback to a GATT characteristic setter; also routes remote BLE writes back to `pattern_controller_change_to_animation`.
- `src/animations/animation_is_active_characteristic.h` — `IsActiveCharacteristic<A>`: a `BtGattAutoCharacteristicExt` subclass that hooks `onWrite` to `AnimationIsActiveBinding<A>::onRemoteActiveChange`.

**DI interfaces (added in `animation-refactor-part2`)**

- `src/bluetooth/bt_state_observer.h` — `BtStateObserver`: pure abstract observer; `bluetooth.cpp` calls through this instead of including `pattern_controller.h` / `bt_animations.h`. Register with `bluetooth_register_state_observer()`.
- `src/configuration_provider.h` — `ConfigurationProvider`: abstract interface over `CoreConfig` singleton (getBrightnessFactor, getDisplayRateMs, getRenderRateMs). `CoreConfig` inherits from it. Injected into `led_controller` and `pattern_controller` via setter functions; lazy fallback to `CoreConfig::getInstance()` if not set.
- `src/button_event_listener.h` + `src/buttons.h` — `ButtonEventListener`: `onButtonPressed(size_t buttonId)`. Dispatch is ISR-safe: GPIO interrupt → `K_MSGQ_DEFINE` → `k_work` → listener on work-queue thread. Register with `buttons_register_listener()`. Button IDs 0–3 = sw0–sw3; ID 4 = wake button.
  - **Physical layout (proto0, a directional grid):** button 0 = Up, button 1 = Left, button 2 = Right, button 3 = Down. The devicetree labels in `boards/others/rgb_sunglasses_proto0/rgb_sunglasses_proto0_nrf5340_cpuapp_common.dts` reflect this (e.g. "Push button 1 (Up)"), but the `sw0`–`sw3` aliases themselves are unchanged. When wiring button behavior for a new animation, use this mapping rather than guessing — e.g. `GlimPlayerAnimation` (`src/animations/glim_player_animation.cpp`) uses button 0 (Up) to advance to the next GLIM file and button 3 (Down) to go to the previous one; buttons 1/2 (Left/Right) are intentionally unassigned there.

**Important: `CoreConfig` getters are non-const.** `getBrightnessFactor()` writes back to clamp the value against the BT characteristic range. Any abstract interface it implements must therefore declare those methods without `const`, otherwise `CoreConfig` becomes abstract and `Singleton<CoreConfig>` fails to instantiate.

### Animation / BT decoupling — COMPLETE

The `animation-refactor-part2` decoupling is **done**: every parameterized animation's GATT service, parameter sources, and is-active wiring live in an adapter under `src/bluetooth/animation_adapters/` (9 adapters as of 2026-07 — re-verify), and no animation `.cpp` includes BT headers. Keep it that way — new animations get a BT-free `.cpp` plus an adapter file; see `/add-animation` for the full add procedure (including the easy-to-miss registration spots). Verify the separation with (from the repo root, like all commands in this file):

```bash
grep -rlE 'bluetooth|BT_GATT|BtGatt' fw/src/animations/
# Matches only comments (adapter cross-references, a BtGattString size note) — no code.
```

`bt_animations.{h,cpp}` (the visual BT-status animations: advertising pulse, connecting flash, pairing code) stay in `src/animations/` intentionally — they render BT state but are driven externally via `BtStateObserver`, and today contain no BT includes themselves. Do not "fix" or relocate them. `fw/docs/animation-bluetooth-decoupling-plan.md` is the historical plan, now executed.

### Other subsystems

- `src/power.cpp` / `drivers/` — TPS25750 USB PD controller (custom driver, patch loaded via LZ4-compressed blob) and BQ25792 battery charger (custom driver). I2C-based.
  - **Power subsystem: safe vs danger.** All `power` shell commands are registered in `src/power.cpp`.
  - **SAFE (read-only)**: `power bq status`, `power bq limits` (ICHG/IINDPM/VINDPM/ICO/watchdog readbacks + DPM status flags — the first stop for "charging too slow" symptoms), `power bq dump`, `power pd dump`, `power pd contract` (negotiated PD contract / Type-C budget + advertised sink caps), `power vreghvout`, and the `bq25792_get_*` driver getters. All `bq25792_get_*` getters propagate I2C errors (negative errno, output untouched) — the legacy ADC/status getters used to swallow bus errors and return stale/zero-but-plausible data, which hid I2Cm bridge outages (fixed alongside the PTCH-wedge recovery work; callers that key on the return value, like the charger status thread's `vbat_ok`/`chg_ok`, rely on this).
  - **DANGER (writes)**: any register write, 4CC task, or patch operation — `power pd patch ...` (`tps25750_download_patch()`), `power pd clear_dbfg`, `power pd go2p` (TRM-cited GO2P task intended to force PTCH mode for recovery testing; refuses to run without a battery present, and hardware-tested 2026-07-17: **cleanly REJECTED on proto0** with PatchConfigSource=6 per TRM Table 3-12 — see `tps25750_go2p()`), the `power bq charge`/`adc`/`pfm`/`freq`/`temp_override` setters (`bq25792_set_*`), and **especially `power boost`**, which writes UICR `VREGHVOUT` — irreversible without a mass chip erase. Every one of these is governed by root `CLAUDE.md`'s "NEVER write unverified commands or data into hardware parts" rule: authoritative datasheet/TRM in hand first, or stop and ask.
  - **A wrong/implausible BQ25792 current or voltage report has two known in-repo root-cause classes** — missing two's-complement sign extension in the ADC decode (IBAT/IBUS are 16-bit signed; fixed in PR #106, regression suite `fw/tests/drivers/bq25792_decode`) and interleaved I2Cm bridge transactions (fixed in PR #111 with the `task_mutex`). Rule both out (see `/debug-fw`'s device-symptoms table) before pursuing any external fix or register write. And remember every BQ25792 register access — the bq25792 DT node is a child of the tps25750 node — goes through the TPS25750 I2Cm bridge's CMD1/DATA1 4CC sequence under that `task_mutex` (`fw/drivers/tps25750/tps25750.c`); any new BQ25792 write inherits that path and its serialization requirements.
  - Prefer exercising power code on native_sim first: `tests/drivers/emul_tps25750` runs the **real** tps25750 + bq25792 drivers against an emulated register file — no hardware, no risk.
- `src/buttons.cpp` — GPIO button handling. Button callback runs in ISR context; dispatch to `ButtonEventListener` is deferred via `K_MSGQ_DEFINE` + `k_work` for thread safety.
- `src/fonts/` — `FontAtlas` and `FontShell` provide bitmap font rendering used by `TextAnimation` and `BtPairingAnimation`.
- `src/sound/sound.cpp` — PDM microphone via VM3011 driver; conditionally compiled with `CONFIG_AUDIO`.
- `src/core_config.cpp` — device-level settings (brightness, display/render thread rates, status LED brightness), each backed by `BtGattPersistentCharacteristic` so they persist via Zephyr's settings subsystem (see below).

### Settings-backed config persistence

Every BT-settable config value (core config, animation parameters/strings/colors, glim selection/loop mode, and the currently-active animation) persists across power cycles via Zephyr's settings subsystem. The storage backend itself (`CONFIG_SETTINGS`/`CONFIG_SETTINGS_NVS`/`CONFIG_NVS`, the `settings_storage` NVS partition on external flash in `pm_static_rgb_sunglasses_proto0_nrf5340_cpuapp.yml`, and the `settings_load()` call in `bluetooth_init()`) predates this and exists for BT bonding — this just adds a second consumer.

- `src/settings/persistent_value_registry.{h,cpp}` — BT-free registry mapping a stable key string → `{target, load_fn, save_fn}`, self-populated by static-init constructors. Lets one shared settings subtree handler dispatch `settings_load()` callbacks to dozens of independently-registered values instead of needing one `SETTINGS_STATIC_HANDLER_DEFINE` per characteristic. Storage is an **intrusive `sys_slist_t` of caller-owned `PersistentValueRegistryEntry` records** (issue #114) — each registrant embeds the entry in its own long-lived object (a characteristic instance's member, an extension `Slot` field, a file-scope static) and passes its address to `persistent_value_registry_register(entry*)`; the registry links it by pointer. Same idiom as Zephyr's own settings backend (`settings_store.c`). There is **no fixed capacity and no `-ENOMEM` path** — a registration can never be silently dropped. Registration must stay single-threaded at static-init/boot (unchanged invariant; list-append isn't concurrency-safe on its own).
- `src/settings/persistent_value_store.{h,cpp}` — owns the single `SETTINGS_STATIC_HANDLER_DEFINE("appcfg", ...)` handler (forwards to the registry's dispatch) and a shared debounced `k_work_delayable`. `request_save()` (re)schedules a flush of every registered value `CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS` after the last call, coalescing rapid writes (typing a string, dragging a color picker) into one flash write. **Don't reuse `CONFIG_BT_SETTINGS_DELAYED_STORE_MS` for this or anything else BT-free** — this module intentionally has its own Kconfig symbol so it has no dependency on the Bluetooth stack.
- `src/bluetooth/persistent_characteristic.h` — see the GATT layer bullet above.
- Bespoke (non-mixin) persistence: `glim_player_animation_bt.cpp` persists the glim selection by **file name**, not index (`glim_registry`'s enumeration order can shift between boots) — and since `glim_registry::init()` runs after `settings_load()`, the loaded name is resolved to an index later, in `glim_player_animation_bind_default_bt_dependencies()`. `pattern_controller.cpp` persists a single "last active animation" key (hooked into `pattern_controller_change_to_animation()`) rather than per-animation booleans, to avoid reconstructing "which one was active" from independent flags.
- **Extension animation parameters** (`extensions/extension_param_persistence.{h,cpp}` + `extensions/extension_host.cpp`, issue #90 follow-up): a third bespoke consumer, same idea as glim — one combined `Blob` (every scalar + string param value) per extension, registered against `persistent_value_registry` under key `"ext/<sanitized displayName>"` (never slot index, since `/NAND:/ext/` file sets can shift between boots) in `scan_slot()`. Extensions hit a real ordering wrinkle glim doesn't: their identity (`displayName`) is only known from the manifest, discovered on the pattern-controller thread — strictly *after* `bluetooth_init()`'s boot-wide `settings_load()` replay has already run — so the registry's automatic dispatch-during-`settings_load()` path can never find these keys. `scan_slot()` therefore calls the new `persistent_value_store::load_value()` (a direct, synchronous `settings_load_one()`, symmetric with the existing `save_value()`) right after registering, instead of relying on the replay. Saves reuse `persistent_value_registry_mark_dirty()` + `request_save()` unchanged, hooked into `extension_host::setParamValue()`/`writeParamString()` exactly like `BtGattPersistentCharacteristic::onWrite()` does for built-ins. **A faulted extension has its persisted params cleared**: `sandbox_fault()` resets `paramValues`/`stringValues` to manifest defaults and synchronously (not via the debounce) overwrites the persisted blob with those defaults, since a bad persisted value could be what caused the crash — without this, both an `ext select` retry and a future reboot would immediately reproduce the same crash from the same poisoned value.
- **`CONFIG_APP_PERSIST_BT_CONFIG`** (default `y`, `n` on `rgb_sunglasses_dk`) gates the whole feature: every call site above is wrapped in `if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG))` (in the template mixin) or `if (IS_ENABLED(...))` (in plain `.cpp` files), so the doLoad/doSave code is fully compiled out and linked away when disabled. This exists because DK's internal-flash image slot has no spare room for it (it was already at ~75% before this feature existed) and DK is legacy / doesn't get new features per the Hardware Revisions note above — disabling needed its own gate rather than `#ifdef`-ing every one of the ~33 characteristic declarations.

### Kconfig and optional features

Animations are conditionally compiled via:

```
CONFIG_ANIMATION_MY_EYES=y
CONFIG_ANIMATION_RAINBOW=y
CONFIG_ANIMATION_ZIGZAG=y
```

App modules are compiled via `target_sources_ifdef(CONFIG_<MODULE> app PRIVATE ...)` lines in `fw/CMakeLists.txt` — that CMake line is the compile gate; in-source `#if DT_HAS_ALIAS(...)` guards (e.g. `led_strip_2` in `fw/src/status_led/status_led.cpp`) are secondary, never the gate. When adding a tunable for an existing module, check that module's `target_sources_ifdef` line first and add `depends on <MODULE>` to the new symbol, matching every other `APP_*` int in `fw/Kconfig` (e.g. `APP_EXT_TICK_DEADLINE_MS` depends on `APP_EXTENSION_HOST`).

Text animation is always compiled. Audio is gated on `CONFIG_AUDIO`. Check `prj.conf` for the full configuration and memory-saving flags (`CONFIG_ASSERT=n`, `CONFIG_CBPRINTF_FP_SUPPORT=n`); `CONFIG_SIZE_OPTIMIZATIONS=y` lives in the proto0 **board** conf, not `prj.conf`.

**No `%f`/`%g` in log or shell format strings.** `CONFIG_CBPRINTF_FP_SUPPORT` and `CONFIG_PICOLIBC_IO_FLOAT` are disabled (~10KB FLASH, issue #79 ROM pass); a `%f` prints the literal specifier instead of the value (no crash). Print floats via integer fixed-point instead — see `fmt_fixed4()` / `agc_gain_db10()` in `src/sound/sound.cpp` (the only float-printing code in the app).

**Don't reuse a Kconfig symbol from one subsystem to configure unrelated code in another, even if the value/semantics happen to line up.** E.g. a BT-free module's debounce/delay tunable should get its own `CONFIG_APP_*` symbol, not borrow `CONFIG_BT_SETTINGS_DELAYED_STORE_MS` just because the timing happens to match — that creates a hidden cross-subsystem dependency and works against this project's general push to decouple BT from non-BT code (see the animation/BT decoupling refactor above).

### SYS_INIT ordering for early registration

`SYS_INIT(fn, APPLICATION, N)` runs before `K_THREAD_DEFINE` threads are scheduled. Lower N runs first. When an observer or listener must be registered before a thread can fire its first event, use `SYS_INIT(APPLICATION, 0)`. Both `bluetooth_init` and `button_init` run at priority 1, so registering observers at priority 0 guarantees the observer is in place before either subsystem starts.

**C++ static constructors run after POST_KERNEL but BEFORE APPLICATION-level SYS_INIT** (`z_static_init_gnu()` in NCS v3.1.1's `zephyr/kernel/init.c` `bg_thread_main`, between the two `z_sys_init_run_level()` calls). Any container that static-init constructors register into — e.g. `src/settings/persistent_value_registry.cpp`, populated by the `BtGattPersistentCharacteristic` / `ChargeEnableCharacteristic` ctors and the `LastActiveAnimationRegistrar` / `GlimPersistenceRegistrar` structs — must be constant-initialized (plain zero-initialized statics, like that file's `sRegistry[]` + `sRegistryCount`), never initialized from an APPLICATION SYS_INIT, which runs after the ctors and would silently discard every registration.

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
- `tests/animations/*_animation_di/` — dependency-injection tests per animation, compiling the pure animation `.cpp` without BT. **No DI suite sets `CONFIG_BT`** (as of 2026-07 the only firmware test suite that does is `tests/bluetooth/battery_service` — re-verify from the repo root with `grep -rln CONFIG_BT=y fw/tests --include=prj.conf`). Coverage is not 1:1: `matrix_code` has no DI suite (as of 2026-07).
- `tests/bt_state_observer/` — interface contract tests for `BtStateObserver` (does not link `bluetooth.cpp`).
- `tests/configuration_provider/` — interface contract tests for `ConfigurationProvider`.
- `tests/power/tps25750_patch_decompression/` — verifies the LZ4-compressed TPS25750 patch round-trips correctly.
- `tests/drivers/emul_bmi270/` — exercises the **real upstream bmi270 driver** on native_sim through the out-of-tree BMI270 emulator at `drivers/emul_bmi270/` (upstream Zephyr has no BMI270 emul; ours is SPI-only, matching proto0). Two Twister scenarios: poll mode, and `CONFIG_BMI270_TRIGGER_OWN_THREAD` where the data-ready trigger is fired by toggling INT2 (**irq-gpios index 1** — the driver maps data-ready to INT2, not INT1) via native_sim's `gpio_emul`. Tests inject SI-unit samples with `emul_sensor_backend_set_channel()` and can peek registers via `emul_bmi270_get_reg()`. `CONFIG_EMUL_BMI270` depends on `EMUL && BMI270` so hardware builds never compile it. Deferred follow-ups: I2C support, bad-chip-id failure path.
- `tests/drivers/emul_tps25750/` — exercises the **real tps25750 + bq25792 drivers** on native_sim through the out-of-tree TPS25750 emulator at `drivers/emul_tps25750/`: the bq25792 node is a DT child of the tps25750 node (same topology as proto0), so every BQ register access runs through the real I2Cm bridge (CMD1/DATA1 4CC tasks + `task_mutex`) into the emulated register file. Two Twister scenarios: default (boots in "APP " mode), and `.patch_download` (`CONFIG_EMUL_TPS25750_BOOT_MODE_PTCH=y` + `CONFIG_TPS25750_INTERNAL_PATCH=y`, asserting the full boot-time PBMs → chunked upload → PBMc flow via received-byte count + FNV-1a hash). Non-obvious mechanics: (1) patch chunks arrive on a **second I2C address** (the DT `patch-address`), which `EMUL_DT_DEFINE` can't cover — the emulator's init hand-registers an extra `struct i2c_emul` for it via `i2c_emul_register()`; (2) the emulated CMD1 must stay **busy for a nonzero window** (`emul_tps25750_set_cmd_delay_ms`) or a bridged transfer contains no blocking point on native_sim, threads never interleave, and the concurrency regression test (for the task_mutex serialization fix) can't reproduce the race — validated by disabling the mutex: 17/100 concurrent reads then return the *other* register's value; (3) the test app needs `list(APPEND DTS_ROOT ...fw)` before `find_package(Zephyr)` or the custom `ti,tps25750`/`ti,bq25792` bindings aren't found; (4) all BQ getters (`bq25792_get_*`) now propagate I2C errors — error-path tests can assert getter errnos directly (driving the bridge with `i2c_burst_read(tps_dev, 0x6B, ...)` also still works).
- `tests/imu/pipeline/` — end-to-end test of `src/imu/imu.cpp` (compiled directly into the test app) against the BMI270 emulator: boot-time ODR/power config, DRDY-driven frames into `imu_result_q`, and the msgq purge-keep-freshest overflow path. Timing gotcha: the bmi270 driver's per-register `k_usleep` delays each round up to a full 10 ms tick on native_sim, so `imu_thread`'s startup config takes ~200 ms of simulated time (the suite setup polls PWR_CTRL for completion) and DRDY pulses need ~50 ms spacing or they coalesce on the driver's trigger semaphore and frames are dropped. `imu.cpp`'s two `fw/Kconfig` symbols (`IMU_THREAD_PRIORITY`/`IMU_THREAD_STACK_SIZE`) are redeclared in the test-local `Kconfig`.

**Pulling out-of-tree drivers + their Kconfig symbols into a standalone test app**: the pattern (used by both `tps25750_patch_decompression` and `emul_bmi270`) is a test-local `Kconfig` containing `source "Kconfig.zephyr"` + `rsource "../../../drivers/Kconfig"`, plus `add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../drivers ${CMAKE_CURRENT_BINARY_DIR}/app_drivers)` in the test `CMakeLists.txt`. No Zephyr module registration needed. Driver dirs use `zephyr_sources()`, not `zephyr_library()` (see the comment in `drivers/vm3011/CMakeLists.txt`).

**Twister `testcase.yaml` naming**: The `name` field must use a dotted `category.name` format (e.g., `interfaces.bt_state_observer`). A plain single-word name causes a `TwisterException` at runtime.

**C++23 in test `prj.conf`**: Use `CONFIG_STD_CPP2B=y` (not `CONFIG_STD_CPP23` — that symbol does not exist). Also add `CONFIG_REQUIRES_FULL_LIBCPP=y` and `CONFIG_REQUIRES_FULL_LIBC=y`.

**Test isolation from heavy dependencies**: If a registration function (e.g., `bluetooth_register_state_observer`) lives in a file with heavy BT stack dependencies, avoid linking that file in unit tests. Test the interface/observer contract directly on a mock implementation without calling the real registration function.

### CONFIG_USERSPACE / kernel-user mode separation (issue #79, proto0 only)

Enabled on proto0 (`CONFIG_USERSPACE=y` in `boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf`). **Not enabled on DK** — its flash budget is already tight (~95% used) and the motivation (future LLEXT animation sandboxing) is proto0-only anyway.

**FLASH cost**: `CONFIG_USERSPACE=y` alone (no threads converted) costs ~105-246KB — mostly `z_vrfy_*`/`z_mrsh_*` syscall verifier functions Zephyr generates for every syscall-covered API already compiled in (GPIO, I2C, SPI, flash, sensor, LED, etc.), regardless of whether anything actually calls them from user mode. This is generated per-Kconfig-enabled-subsystem, not per-actual-usage — **there is no way to selectively emit syscalls for only the subsystems a converted thread needs**; confirmed by reading `parse_syscalls.py`/`gen_syscalls.py`/`syscall_dispatch.c`. `CONFIG_EMIT_ALL_SYSCALLS` only widens emission further, never narrows it. Fitting this required a dedicated flash-reduction pass first: `CONFIG_DUMP_DEVICE_REGISTERS=n` (~94KB, see the comment at its Kconfig line), `CONFIG_FLASH_SIMULATOR_STATS=n` in `prj.conf` (~4KB), and a `tuple_cat` collapse in `bt_service_cpp.h` (see that file's comment).

**Second ROM pass (issue #79 follow-up)** took the appcore from 94.6% to **64.6% FLASH** (624,492 B of the 966,144 B `app` slot), measured deltas:
- `CONFIG_SIZE_OPTIMIZATIONS=y` replacing `CONFIG_DEBUG_OPTIMIZATIONS=y` (proto0 board conf): **~255KB**. The entire image had been compiling `-Og`. Flip back temporarily for deep GDB sessions if needed.
- Shell pruning (`CONFIG_SENSOR_SHELL=n`, `CONFIG_FLASH_SHELL=n`, `CONFIG_DEVMEM_SHELL=n`): **~29KB** FLASH + ~21KB RAM.
- Float printf removal (`CONFIG_CBPRINTF_FP_SUPPORT=n`, `CONFIG_PICOLIBC_IO_FLOAT=n`, after converting `sound.cpp` to integer prints): **~4.9KB**.
- Deliberately kept: LLEXT (+shell/EDK, ~20-30KB — groundwork for the loadable-extensions branch), DEBUG_COREDUMP (feature planned), CMSIS-DSP (all four sub-options genuinely used by `audio_dsp.cpp`: rfft_fast/cmplx_mag_squared/mean/std/hanning).
- Biggest remaining single item: `bt_service_cpp.h` template instantiations, **~70KB** of `fw/src`'s ~152KB (per-service `BtGattServer<...>` constructors + `tupleToArray` expansions). Recovering it means building the `bt_gatt_attr` tables at runtime instead of per-service templates — a separate, riskier refactor (tracked as a follow-up issue).

**`imu_thread` (`src/imu/imu.cpp`) is the first (and so far only) thread converted to `K_USER`.** Two real, non-obvious crashes were hit converting it — both root-caused via GDB+SWD (USB never enumerates when either crash happens at boot, so serial logs aren't available):

1. **`K_THREAD_DEFINE` + `k_mem_domain_add_thread()` crashes on this SoC.** This SoC config has `CONFIG_ARCH_HAS_CUSTOM_SWAP_TO_MAIN=1`, which means `K_THREAD_DEFINE`-created (static) threads are set up with `_current == NULL` — this skips `z_mem_domain_init_thread()` and leaves the thread's `mem_domain_info` permanently zeroed (never linked into `k_mem_domain_default` or any domain). `k_mem_domain_add_thread()` unconditionally tries to unlink the thread from its prior domain first (`remove_thread_locked()` → `sys_dlist_remove()`), which faults on that never-linked list node. **Fix: create the thread dynamically instead** — `K_THREAD_STACK_DEFINE` + `struct k_thread` + `k_thread_create(..., K_FOREVER)` called from a SYS_INIT hook (so `_current` is a real, already-domain-linked thread), do the access-grant/domain setup, then `k_thread_start()`. Matches the pattern in Zephyr's own `samples/userspace/prod_consumer/src/app_a.c`. This is a project-wide fact, not specific to `imu_thread`: **every** `K_THREAD_DEFINE` thread in this project has this same zeroed `mem_domain_info` (confirmed via GDB on `status_led_thread` too) — any future thread conversion needs the same dynamic-creation treatment, not just a `K_USER` flag added to its existing `K_THREAD_DEFINE`.
2. **A converted thread also needs `z_libc_partition`, not just its own partition.** `z_arm_tls_ptr` (the current thread's TLS pointer, read by literally every thread at entry via `__aeabi_read_tp()` since `CONFIG_CURRENT_THREAD_USE_TLS=y` here) lives in `z_libc_partition`, which is part of `k_mem_domain_default` by default — so every thread has it for free until it's moved to a custom domain. Moving a thread to its own memory domain (as any `K_USER` conversion does) silently drops that access unless `z_libc_partition` (`#include <zephyr/sys/libc-hooks.h>`) is added to the new domain's partition list alongside the thread's own partition. Without it: a usage fault on the very first instruction of `z_thread_entry()`, before the thread's own entry function even starts.

See `src/imu/imu.cpp`'s `imu_init()` for the working reference implementation of both fixes.

**Threads still blocked from conversion** (missing syscall coverage in NCS v3.1.1, confirmed by grepping `__syscall` across the actual headers this project uses, not inferred): the entire Bluetooth host stack, the USB device_next stack, mcumgr, the settings subsystem, the filesystem API (`fs_*`), `flash_area_*`, `dmic_*`, and the WS2812 `led_strip_update_rgb()` driver call all have **zero** `__syscall` wrappers. `bt_thread`, `audio_dsp_thread` (also does raw MMIO register pokes for AGC gain), the MCUboot updater work queue, and the settings-save work queue should stay kernel-mode indefinitely barring upstream NCS syscall additions (per the "never touch the NCS SDK" rule). `led_display_thread`/`status_led_thread` are conceptually simple but blocked on `led_strip_update_rgb` not being a syscall — would need one small custom project-defined syscall wrapper shared by both. `pattern_controller_thread` (the actual LLEXT-motivated target) needs its FS (`glim_registry::init()`) and settings-persistence (`persistent_value_store::request_save()`) calls hoisted out into an IPC/message-passing call to a kernel-mode helper first, since those can't be called directly from user mode.

**MPU region budget**: nRF5340's Cortex-M33 MPU has 8 hardware regions; 2 are permanently consumed by Zephyr's default flash/RAM background map, leaving ~6 dynamic regions (~4-5 usable partitions per active memory domain in practice) — a real constraint for anyone adding more domains.

**Note the old per-thread conversion plan above is superseded for the LLEXT case** by the extension-host design (issue #85, implemented below): only extension code runs in user mode; `pattern_controller_thread`/`led_display_thread`/`status_led_thread` stay kernel-mode, so no `led_strip_update_rgb` syscall or FS-hoisting is needed.

### Sandboxed animation extensions (issue #85, `src/extensions/` + `fw/extensions/`)

`.llext` animation extensions are discovered at boot from `/NAND:/ext/` and executed **exclusively on one K_USER sandbox thread** confined to a single shared memory domain re-initialized per activation (`z_libc_partition` + llext's 4 TEXT/RODATA/DATA/BSS partitions = 5, hardware-verified to fit the MPU budget). The kernel-side pattern controller exchanges data purely through the extension's own exported globals (ABI in `include/rgbx/rgbx_api.h` — 16 params of type UINT32/COLOR/BOOL/STRING, IMU + audio + button inputs; C++ wrapper `include/rgbx/rgbx_animation.h`), enforces a per-tick deadline (`CONFIG_APP_EXT_TICK_DEADLINE_MS`), and recovers from hangs/faults by tearing the sandbox down. Extensions appear as first-class animations: runtime GATT services (`extension_bt.cpp`, `CONFIG_BT_GATT_DYNAMIC_DB=y`, ids `0x40 + slot`, capacity/ID constants + static_asserts in `extension_limits.h`) that the app renders with zero app-side changes, including the bulk metadata characteristic (`extension_metadata_blob.{h,cpp}`, issue #90 follow-up): a runtime-built mirror of `bt_service_cpp.h`'s compile-time `MetadataBlobBuilder` — same wire format, same fixed UUID/version, threaded through `extension_bt.cpp`'s existing `append_characteristic()` helper so blob order can never drift from GATT handle order. Gated by the same `CONFIG_APP_BT_METADATA_CHARACTERISTIC` symbol as the compile-time mechanism, though the cost here is a few hundred bytes of RAM per extension slot, not flash (no template instantiation). Manifest validation is a pure function (`extension_manifest.cpp`, covered by the `extensions.manifest` native_sim suite) — every manifest-embedded pointer is untrusted and bounds-checked before any kernel-mode dereference. Developer docs: `fw/extensions/README.md`; API docs: `doxygen fw/extensions/Doxyfile`. Non-obvious facts learned the hard way:

- **Load-on-activate**: boot discovery loads each ELF transiently (validate + copy metadata), then unloads; only the ACTIVE extension is llext-resident. `activate()` (often on the BT RX thread) only queues the load — the pattern-controller thread performs the FAT read + relocation + sandbox bring-up lazily on the first `tick()`, so an `rgbx_init` failure is reported *asynchronously* (fault + Is Active notify), not as an `activate()` return value. Consequence: the heap only needs the largest single extension (`CONFIG_LLEXT_HEAP_SIZE=24` KB) and 16 slots fit.
- **The llext heap buffer is `.noinit` but IS counted in the linker's RAM percentage** (verified in zephyr.map — don't "discover" 64 KB of free RAM that isn't there).
- **`k_sys_fatal_error_handler` is overridden in `extension_host.cpp`** (root-caused via GDB+SWD): Zephyr's default weak handler halts the WHOLE system on any fault — z_fatal_error() only demotes to a thread abort if the handler *returns*. The override returns only for faults on the sandbox thread; all other faults keep the stock halt-for-GDB behavior. Without it, an extension MPU fault parked the CPU in `arch_system_halt()`.
- **C++ extensions require a partial link** (`ld -r`, done by `fw/extensions/build.sh`): COMDAT group sections (`.text._Z...`) interleave with `.data`/`.bss` file offsets in a single object and fail llext's region-overlap check ("Region 0 ELF file range ... overlaps with 1").
- **The `llext-edk` cmake target does not rebuild when headers change** — delete `build/fw/zephyr/llext-edk.tar.xz` first (build.sh does).
- **Extension init arrays run inside the sandbox** via `llext_bringup()` from the user-mode thread entry (`llext_get_fn_table` is a syscall) — needed for C++ static constructors, though GCC constant-initializes simple instances (vtable pointer via `.rel.data`).
- Re-initializing the shared `k_mem_domain` is safe **only after the sandbox thread is aborted** (`k_mem_domain_init` fully resets the object; `k_thread_abort` unlinks the thread) — every teardown path preserves that order.
- Debug shell: `ext list` / `ext select <slot>` / `ext param <slot> <idx> [<value>]` (type-aware: bools 0/1, strings as text) / `ext stats` (tick-handshake min/avg/max µs). The hello kitchen-sink demo doubles as the recovery test (`Crash`/`Hang` bool params).
- **Animations must render near full-scale (255) channel values**: the pattern controller multiplies every pixel by the global brightness factor (default 20/1000 = 0.02), so a "dim" animation drawing at 32/255 is invisible on the panel. This looked like a crash on the original hello demo — it was just arithmetic.
- **Fault recovery is deliberate**: a dead sandbox is unloaded, un-marks + notifies the animation's Is Active characteristic (app toggle turns off), scrolls a `FAULT: <name>` banner on the panel (proxy), and BLE re-activation is rejected; only `ext select <slot>` clears the fault and retries. The host serializes activate/deactivate/tick/param-writes with a mutex — `pattern_controller_change_to_animation()` runs synchronously on the *caller's* thread (BT RX for GATT writes, shell), so nothing here may assume pattern-controller thread context.

### Scope reminder

Prefer changes under `fw/` (app code, relative to the current repo/worktree root). Only touch `/root/ncs/v3.1.1` (NCS SDK) when explicitly requested.

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

**Hold the `board` hardware lock before connecting.** Another agent in a different worktree may be flashing, resetting, or already talking to this same board. Run `Monitor(command: "scripts/hw-lock.sh hold board", persistent: true)` (see root `CLAUDE.md` "Hardware locking") before opening any `mcp__serial__*` connection here — a `PreToolUse` hook auto-denies these calls without it, so hold it proactively rather than finding out from a denial mid-flow.

**Graduate working shell interactions into serial MCP plugins.** Once you've figured out how to reliably drive a shell subsystem over raw `serial_write`/`serial_read_until` — correct command syntax, response parsing, any device-specific quirks — don't keep repeating that raw sequence in future sessions. Write or extend a plugin under `.serial_mcp/plugins/` (use `serial_plugin_template` to scaffold, `serial_plugin_load`/`serial_plugin_reload` to pick it up) so the next interaction is a single typed tool call instead of hand-rolled read/write. `rgb_sunglasses.py` (see below) is the first instance of this pattern, for the `anim` subsystem — add new plugin files (or new tools in the existing one) the same way for other shell subsystems as they come up.

**Wait for boot before sending commands.** Boot log output interleaves with shell echoed input and causes `command not found` errors. Wait until `uart:~$` appears before issuing any shell commands.

**Sending newlines correctly.** `serial_write` with `data: "\r\n"` sends the four literal characters `\`, `r`, `\`, `n` — not a CR+LF. The same applies to every escape sequence: `"\x03"` sends four literal characters, NOT Ctrl+C — and those literal bytes land in the shell's line editor and corrupt the next command (`command not found` on otherwise-correct input; recover with flush + resend). To send control characters use the `as: "hex"` form or the `rgb_sunglasses` plugin's commands (which handle Ctrl+C internally). Always use one of these instead:

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

**Old boot logs flushing on port-open look like a spontaneous reboot — they aren't.**
The USB CDC shell buffers unread output while no terminal is attached; freshly
opening the port can dump a backlog that starts with `[00:00:00.xxx]` boot logs
from a reset that happened minutes earlier. Before concluding the board just
rebooted (or crashed), run `kernel uptime` — if uptime is large, you're reading
backlog, not a fresh boot.

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
bt_state                 # SNAPSHOT of BLE link health: state (advertising/connected),
                         # peer addr, security level, negotiated ATT MTU, conn params.
                         # ALWAYS RUN THIS FIRST when debugging a BLE connection that
                         # looks stuck (see the "Debugging a stuck BLE connection" note
                         # below) - it distinguishes the split-brain in one command.
bt_conn_info             # print the *actual* current LE connection interval/latency/timeout
                         # (see bluetooth.cpp's le_param_updated callback for the issue #41
                         # connection-interval investigation this was added for)
mcuboot_version         # read MCUboot version from retention registers (major.minor.rev+tweak)
mcuboot_update verify   # read /NAND:/mcuboot.bin, print GRMB header fields, compute and compare CRC
mcuboot_update sideload # open /NAND:/mcuboot.bin and validate it (no BLE upload needed)
mcuboot_update commit   # flash validated package to internal MCUboot region and reboot
mcuboot_update request_reboot  # set gpregret2=BOOT_MODE_REQ and reboot (MCUboot skips fprotect)
fatfs reformat          # nuke and recreate the NAND FAT filesystem (all files erased)
```

**Debugging a stuck BLE connection ("split-brain") — run `bt_state` FIRST.** The
classic symptom is the board's status LED solid (not breathing) while the companion
app reports a connection failure/timeout: the board thinks it's connected, the app
doesn't. `bt_state` (`src/bluetooth.cpp`, added issue #90) prints the whole picture
in one shot — state, peer address, **security level**, and the **negotiated ATT
MTU** — so you don't have to reconstruct it from a two-sided native `adb logcat`
BLE trace. The decisive tells:
- `Security level: L1 (UNENCRYPTED)` on a connection that's been up more than a
  second → LE Secure Connections pairing stalled (often a passkey dialog waiting on
  the phone — see the root `CLAUDE.md` pairing note).
- `ATT MTU: 23 (DEFAULT - MTU exchange did not complete)` on a `CONNECTED` + `L4`
  link → **this is the split-brain**: the phone's GATT stack is wedged (it never
  completed the ATT MTU exchange, so its discovery is hung too). The usual root
  cause is a **stale bonded GATT cache** after a firmware reflash that changed the
  GATT layout. Whether the phone auto-recovers is **device-dependent** (hardware
  proven, issue #90): a **Pixel 9 Pro (stock Android)** honors the firmware's
  Service Changed indication and re-discovers on its own (no split-brain), but the
  shared **OnePlus 9 Pro (OxygenOS)** does **not** — it hangs, and the only fix is
  **forget the device on the phone and re-pair**. So this MTU-23 hang is expected on
  the OnePlus after any GATT-changing reflash, and NOT expected on a Pixel.
  Resetting the *board* (`kernel reboot warm`) clears the board's half of the
  split-brain so it advertises again, but does not fix the phone's stale cache.

**`storage` is a reserved macro in NCS** — `nrf/include/flash_map_pm.h` defines `#define storage settings_storage` (conditionally). The `UTIL_CAT` macro used inside `SHELL_CMD_REGISTER` double-expands its arguments, so `SHELL_CMD_REGISTER(storage, ...)` silently registers a command named `settings_storage` instead of `storage`. Use `fatfs` (or any other token not in `flash_map_pm.h`) for shell commands related to the FAT disk.

**MCUboot VERSION incremental build** — editing `fw/sysbuild/mcuboot/VERSION` alone does NOT trigger ninja to recompile. Force a rebuild of the version-stamped objects by deleting `fw/build/mcuboot/CMakeCache.txt` (forces cmake reconfigure) and then touching `fw/build/mcuboot/zephyr/include/generated/zephyr/app_version_override.h` (forces recompile of `boot_record.c.obj` and `banner.c.obj`).

**Serial connection pool limit** — the MCP serial server defaults to 10 concurrent connections. After several J-Link flashes + reboots, ttyACM ports accumulate and connections are never GC'd. When you hit the limit, close all stale connections explicitly before opening the new port.

**ttyACM port numbers shift after every reboot or J-Link flash** — the device re-enumerates and Linux assigns the next available minor numbers. After each reset: loop over `/sys/class/tty/ttyACM*`, create any missing `/dev/ttyACMN` nodes with `mknod`, then probe each new port with Ctrl+C to find the shell (look for `uart:~$`). Verify a stale node is actually current by comparing `cat /sys/class/tty/ttyACMN/dev` (major:minor) against `ls -la /dev/ttyACMN`.

**TPS25750 log fires at ~10 ms after boot** — the USB PD controller always logs `tps25750: MODE is not PTCH (got APP) Cannot download patch!` around 10 ms uptime. If the first shell command sent after boot is read with `read_until("uart:~$")`, this log fires first, matches the redraw prompt, and swallows the command's actual output. Fix: flush the RX buffer and resend the command; the second call completes cleanly.

## USB Flash Disk (`/NAND:` — GLIM/animation assets)

The dev board exposes a ~6.9 MiB FAT filesystem over USB Mass Storage (SCSI Bulk-Only,
interface 4 of the composite USB device). This is the "NAND" disk Zephyr mounts at
`/NAND:` (`src/storage/storage.cpp`; LUN registered in `src/usb/usb_init.c` as
`USBD_DEFINE_MSC_LUN(nand, "NAND", "RGB-SG", "FlashDisk", "0.00")`). It's how
`bad_apple.glim`, `nyan_cat.glim`, and similar assets get onto the device (see
`fw/tools/convert_bad_apple.py`, `generate_nyan_cat_glim.py`).

**GLIM format**: `src/storage/GLIM_FORMAT.md` is the normative spec, with `src/storage/glim_decoder.{h,cpp}` as the reference implementation; converters live in `fw/tools/` and are gated by CI's `python-tests` job (run locally: `cd fw && pytest tools/tests/ -v`). `fw/scripts/img_to_c.py` is a broken stub (it never writes any output) — do not use it.

This is exclusive-write access to the board's disk — hold the `board` lock first (`Monitor(command: "scripts/hw-lock.sh hold board", persistent: true)`) if doing this by hand instead of via `/provision-device` (which enforces it for you). The hook can't catch an ad-hoc `mount` command — this remains convention-only.

**Finding and mounting it from the devcontainer:**

```bash
# It enumerates as a SCSI disk alongside the container's own disks — identify it
# by the SCSI string, not a fixed /dev/sdX (the letter shifts based on what else
# is attached).
dmesg | grep -A2 "RGB-SG"       # confirms detection, e.g. "scsi 1:0:0:0: Direct-Access RGB-SG FlashDisk"
lsblk                            # cross-reference the ~6.9 MiB size to find the device node, e.g. /dev/sdg
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

**FAT concurrent access causes read corruption.** The firmware mounts the FAT volume at boot and caches cluster allocations. If you write a file over USB while the firmware still has the volume mounted, the firmware's in-memory FAT doesn't know about the new cluster chain — subsequent reads return stale data (wrong CRC, wrong file content). Always write via USB → sync → umount → **reboot the device** before reading the file from firmware. A warm reboot (`kernel reboot warm`) is sufficient; no J-Link needed. This also applies to `mcuboot.bin` staging.

**Reformatting the NAND filesystem from the shell**: use `fatfs reformat` (requires the firmware to be built with `CONFIG_FILE_SYSTEM_MKFS=y`, which is already on for proto0). This is the correct fix for FAT corruption. After the reformat you must reboot the board and re-copy any files you need.

## Coredumps (issue #80, proto0 only)

A fatal fault captures a Zephyr coredump to the 64 KB internal-flash `coredump_partition`
(0xF0000) via the NCS `DEBUG_COREDUMP_BACKEND_NRF_FLASH_PARTITION` backend — raw
`nrfx_nvmc` pokes, the only flash path that works inside the fault handler (IRQs locked;
the external QSPI driver needs interrupts/scheduler, so dumps can NEVER target external
flash directly). `z_fatal_error()` writes the dump BEFORE calling
`k_sys_fatal_error_handler`, so extension-sandbox faults produce dumps too even though
the handler demotes them to a thread abort.

**Post-fault behavior** (`k_sys_fatal_error_handler` in `src/extensions/extension_host.cpp`):
sandbox faults → thread abort as before; anything else → cold reboot, UNLESS a debugger
is attached (DHCSR C_DEBUGEN), in which case it halts for GDB as before. Expect a ~2 s
freeze during capture (16-page partition erase + write, IRQs locked) — including on
recoverable sandbox faults.

**Drain + reminder** (`src/debug/coredump_manager.cpp`, `CONFIG_APP_COREDUMP_MANAGER`):
every `CONFIG_APP_COREDUMP_REMINDER_PERIOD_S` (60 s) a dedicated workqueue checks the
partition, copies any verified dump to `/NAND:/coredump/core_NNNN.bin`, invalidates the
partition, and logs a `LOG_WRN` reminder while any `core_*.bin` remains on disk. Delete
the files (e.g. `coredump-fetch.sh --delete` + board reboot) to stop the reminder. The
pure logic lives in `coredump_manager_core.cpp` behind a `PartitionOps` seam so
`tests/debug/coredump_manager` covers it on native_sim (where `DEBUG_COREDUMP` doesn't
exist).

**Fetch + debug from the host:**

```bash
fw/scripts/coredump-fetch.sh --delete ./dumps   # mount MSC disk, copy core_*.bin off
fw/scripts/coredump-debug.sh dumps/core_0000.bin  # gdbserver --pipe + arm-zephyr-eabi-gdb, prints bt
```

The dump files are the raw Zephyr coredump stream ("ZE" magic) that
`coredump_gdbserver.py` consumes directly. The ELF passed to coredump-debug.sh must be
from the build that produced the crash. Serial fallback when USB is unavailable:
`coredump print` on the shell, then `coredump_serial_log_parser.py` on the captured log.
The built-in `coredump find/verify/print/erase` shell commands are enabled on proto0.

**Test commands**: `crash panic` (kernel panic) and `crash mpu` (write to RO flash →
MemManage fault), `CONFIG_APP_CRASH_TEST_COMMANDS`. Full loop: `crash panic` → reboot →
within ~5 s the manager logs `coredump ... saved to /NAND:/coredump/core_0000.bin` →
fetch + debug → GDB backtrace shows `cmd_crash_panic` on the shell thread.

**Dump-size budget — 64 KB is a hard cap, not a truncation point.** The NCS backend
drops the ENTIRE dump if it doesn't fit (`-ENOMEM`, header never written, nothing to
find on reboot). The budget is enforced by `DEBUG_COREDUMP_THREAD_STACK_TOP_LIMIT=1536`
(each thread's stack dumped only from SP down, capped at 1536 B) — worst case ~55 KB at
~27 threads; the arithmetic lives next to the Kconfig in
`boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf`. Redo it before raising the limit or
adding many threads.

**DK**: coredump support was dropped entirely (`DEBUG_COREDUMP` left `prj.conf` for the
proto0 board conf) — no partition, no spare flash, legacy board.

## Flashing via J-Link (fast path)

When `/check-hardware` reports the J-Link `Status: OK`, prefer flashing over it instead of the slow MCUmgr/UART upload path below.

`jlink-flash.sh` refuses to run unless this session holds the `board` hardware lock (`Monitor(command: "scripts/hw-lock.sh hold board", persistent: true)`) — see root `CLAUDE.md` "Hardware locking". If you're iterating (build → flash → check behavior over `mcp__serial__*` → adjust → rebuild → reflash), hold the lock across the whole cycle rather than releasing between passes — release once the iteration is actually done, not preemptively between steps you're about to repeat.

```bash
fw/scripts/jlink-flash.sh                  # uses fw/build by default
fw/scripts/jlink-flash.sh /path/to/build    # explicit build dir
fw/scripts/jlink-flash.sh -- --skip-rebuild # extra args forwarded to `west flash`
```

`jlink-flash.sh` auto-detects the attached J-Link's serial number and runs `west flash -d <build-dir> --dev-id <serial>` — no need to hardcode or look up `--dev-id` yourself. `/check-hardware` also prints the serial directly under the J-Link section (`Serial: ...`) if you need it for some other tool.

- This triggers a `west build` rebuild-check first (fast no-op if nothing changed), then flashes via the **`nrfutil` runner** (not raw `JLinkExe`) — it programs both `merged_CPUNET.hex` (netcore) and `merged.hex` (appcore), each with erase → program → verify → reset.
- Typical total time: ~30-45s, plus ~15s for USB re-enumeration afterward. Re-run `/check-hardware` to confirm both ttyACM ports are back before issuing further serial/mcumgr commands.
- This is the only way to reflash the bootloader (MCUboot/b0n); MCUmgr can only update the application images.
- **The default build dir is resolved relative to the script's own location, not the caller's cwd or the main checkout** (`REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"`), so `fw/scripts/jlink-flash.sh` with no arguments correctly uses *this* worktree's `fw/build` when run from a worktree — no need to pass the build dir explicitly.

### J-Link "Cannot connect" / nrfutil "Failed to open connection": run fix-usb-dev-nodes.sh

Almost always a missing (or bogus 0-byte regular-file) `/dev/bus/usb` node after re-enumeration — the devcontainer has no udev. **Rule: run `fw/scripts/fix-usb-dev-nodes.sh` before every J-Link flash attempt and again after the board re-enumerates**; a failed flash → fix → retry cycle converging on the second attempt is normal. Full symptom table — including the distinct APPROTECT/debug-port lockout and its `nrfutil device recover` procedure — lives in `/debug-fw`.

### Recovering a wedged shell UART without reflashing

If the shell UART (ttyACM0, `cdc_acm_uart0`) stops accepting writes (host-side `serial.write` times out) while the other CDC interface (ttyACM1, mcumgr) still works, the shell thread is likely wedged — e.g. a `sensor stream <dev> on ...` left running combines with `hw-flow-control`'s blocking `poll_out` to starve the OUT endpoint. This is a firmware hang, not a USB/WSL2 dropout (confirm via `lsusb | grep 2fe3` — device still enumerates).

Don't reach for a full `jlink-flash.sh` reflash for this — it's slower and reprograms flash unnecessarily. Just reset the target CPU over the J-Link's SWD connection:

```bash
nrfutil device reset --serial-number <jlink-serial> --reset-kind RESET_PIN
```

(`<jlink-serial>` is the same S/N `/check-hardware` and `jlink-flash.sh` print, e.g. `50104975`.) This power-cycles/resets the target without touching flash contents. The board re-enumerates over USB afterward the same as after any reset — poll `lsusb`/`/dev/ttyACM*` before issuing further commands.

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

### Firmware update flow (OTA via MCUmgr)

`image upload` the signed image (`fw/build/fw/zephyr/zephyr.signed.bin`, ~3-4 min over serial), `image test <hash>`, `reset`, then `image confirm` after a good boot — MCUboot auto-reverts if the test boot fails. The step-by-step procedure (with re-enumeration handling) lives in `/flash-and-verify`; prefer the J-Link fast path above when a J-Link is attached.

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

**A newly-added overlay file may not be picked up without a `--pristine` rebuild.** Zephyr's overlay auto-discovery (`zephyr_file(CONF_FILES ... DTC_OVERLAY_FILE ...)`) is gated behind `if(NOT DEFINED DTC_OVERLAY_FILE)` — if a prior configure already ran and cached `DTC_OVERLAY_FILE` (even as an empty string) in `build/<image>/CMakeCache.txt`, the auto-discovery block is permanently skipped on every subsequent incremental build, even after adding the right file in the right place. If a new overlay doesn't seem to take effect, check `grep DTC_OVERLAY_FILE build/<image>/CMakeCache.txt` — if it's defined-but-empty, do a full `--pristine` rebuild instead of debugging the overlay content. Deleting just `build/<image>/CMakeCache.txt` (as in the MCUboot VERSION-file trick elsewhere in this file) has NOT been validated for overlay rediscovery — use the full `--pristine` rebuild.

## MCUboot and LED data pins (GPIO retention across warm resets)

MCUboot never links the SPI/LED_STRIP drivers, so the 3 WS2812 data-in pins (P0.29, P1.05, P1.01 — see `fw/docs/proto0-board-pinout.md`) are left completely unmanaged during MCUboot's runtime. On the nRF53, GPIO peripheral state (direction/level) is retained across a CPU/software (warm) reset — only a power-on/brownout reset clears it. So if the app was driving a data line high before a warm reset (e.g. the `sys_reboot(SYS_REBOOT_WARM)` that follows MCUmgr's `image test`/`reset`), MCUboot inherits that stuck-high state, and a WS2812 strip can read it as a steady "on" signal and pull max-brightness current for the entire bootloader boot window — risking a brownout that prevents boot entirely.

Fixed via Zephyr's built-in GPIO hogs feature (`CONFIG_GPIO_HOGS`, auto-enabled by the presence of `gpio-hog` devicetree nodes): see `fw/sysbuild/mcuboot/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.overlay`. It forces all 3 pins to a driven-low GPIO output very early in boot (`SYS_INIT` priority 41), independent of any driver, for MCUboot's entire runtime. If you add more LED data pins in future hardware revisions, extend this overlay too.
