---
paths:
  - "fw/src/bluetooth/**"
  - "fw/src/bluetooth.cpp"
  - "fw/src/pattern_controller_bt_observer.*"
  - "fw/tests/bluetooth/**"
  - "fw/tests/bt_state_observer/**"
  - "fw/tests/pattern_controller/**"
---

# Firmware Bluetooth / GATT layer

Loads when you read firmware BLE code. The wire contract shared with the app (refusing writes,
metadata blob, notify-vs-MTU, connection interval, UUIDs) is `.claude/rules/ble-gatt-contract.md`;
adding a characteristic is `/add-gatt-characteristic`; BLE symptoms are `/debug-ble`.

## `fw/src/bluetooth.cpp` — BT thread + state machine

- IDLE → ADVERTISING → CONNECTING → CONNECTED. Uses `K_MSGQ` to decouple connection callbacks
  from state transitions. Requires `BT_SECURITY_L4` before transitioning to CONNECTED. Pairing is
  Display-only: the `passkey_display` auth callback prints the passkey on the serial console
  (`/re-pair` and `.claude/skills/re-pair/references/manual-pairing.md`).
- Connection-parameter requests belong to the `CONFIG_APP_BT_CONN_PARAM_GOVERNOR` state machine
  (see the contract rule for the FAST/MEDIUM/SLOW policy); the direct `bt_conn_le_param_update()`
  just before CONNECTED is the governor-off fallback.
- **`bt_conn_info` shell command and `le_param_updated` connection callback (issue #41
  follow-up)** exist to verify, rather than assume, what connection interval is in effect —
  `bt_conn_le_param_update()` only sends a _request_. `le_param_updated(conn, interval, latency,
  timeout)` (in `conn_callbacks`, a `BT_CONN_CB_DEFINE`) logs the real negotiated parameters
  every time they change, with a timestamp, so you can see exactly when a request converged
  relative to other events. `bt_conn_info` (a standalone `SHELL_CMD_REGISTER`, no subcommands)
  prints the _current_ parameters of `s_active_conn` (a diagnostic-only tracked pointer,
  ref-counted via the `connected()`/`disconnected()` callbacks) on demand — useful for polling
  mid-connection from a second shell session while the app is mid-discovery.
- **`bt_state`** (added issue #90) prints the whole link picture in one shot — state, peer
  address, security level, negotiated ATT MTU, connection parameters. Run it FIRST when a BLE
  connection looks stuck; `ATT MTU: 23` on a `CONNECTED`/`L4` link is the stale-GATT-cache
  split-brain (`/debug-ble`).

## `fw/src/bluetooth/bt_service_cpp.h` — compile-time GATT server assembler

- `BtGattServer<Providers...>` collects `BtGattAttributeProvider` objects, assigns auto UUIDs in
  provider-declaration order, and flattens them to a `bt_gatt_attr[]` backed by a `std::array`.
  Register with Zephyr via `BT_GATT_SERVER_REGISTER(name, server)`.
- Characteristic aliases: `BtGattReadWriteCharacteristic`, `BtGattReadNotifyCharacteristic`,
  `BtGattAutoReadWriteCharacteristic`, etc.
- Write hooks: if a characteristic class defines `onWrite(const T&)`, it is called after each
  successful remote write. **`int onWriteChecked(const T&)`** is the fallible variant: a non-zero
  return restores the previous value and fails the ATT write with `WRITE_REQ_REJECTED` — use it
  when accepting the write depends on a side effect that can fail (e.g. an I2C register write).
  Define one or the other, not both; `onWriteChecked` is not supported for string-backed types.
- The bulk metadata characteristic this file synthesizes, and the `notify()` length rules, are in
  the contract rule.
- `bt_service_cpp.h` template instantiations are the biggest single FLASH item in `fw/src`
  (`.claude/skills/rom-ram-budget/references/rom-pass-history.md`); a `tuple_cat` collapse in this
  file was part of the pass that made `CONFIG_USERSPACE` fit (see the comment there).

## Persistence and Is Active plumbing

- `fw/src/bluetooth/persistent_characteristic.h` —
  `BtGattPersistentCharacteristic<Key, Description, Notify, T, Default, OnRemoteWrite>` (the last
  parameter defaults to `&bt_gatt_no_write_hook<T>`): a `BtGattAutoCharacteristicExt` subclass that
  backs a POD/`BtGattColor`/`BtGattString<N>` characteristic with Zephyr's settings subsystem.
  `Key` is an explicit string literal (e.g. `"core/brightness"`) — never derive it from
  declaration order, since auto-UUIDs are positional but settings keys must stay stable across
  reorderings. Mechanism: `.claude/rules/fw-settings-persistence.md`. `BtGattDropdownList<N>`
  characteristics (glim selection/loop mode) don't fit this mixin and persist by hand — see
  `fw/src/bluetooth/animation_adapters/glim_player_animation_bt.cpp`.
- `fw/src/bluetooth/animation_is_active_characteristic.h` — `IsActiveCharacteristic<A>`: a
  `BtGattAutoCharacteristicExt` subclass that hooks `onWrite` to
  `AnimationIsActiveBinding<A>::onRemoteActiveChange` (the BT-free binding in
  `fw/src/animations/animation_is_active_binding.h`).
- Animation GATT services live in adapters under `fw/src/bluetooth/animation_adapters/` — one per
  parameterized animation; see `.claude/rules/fw-animations.md` for the decoupling rule.

## `BtStateObserver`

`fw/src/bluetooth/bt_state_observer.h` — pure abstract observer; `bluetooth.cpp` calls through it
instead of including `pattern_controller.h` / `bt_animations.h`. Register with
`bluetooth_register_state_observer()`; the pattern controller's observer registers at
`SYS_INIT(APPLICATION, 0)` (`fw/src/pattern_controller_bt_observer.cpp`) so it is in place before
`bluetooth_init` (priority 1) starts. `fw/tests/bt_state_observer/` tests the contract without
linking `bluetooth.cpp` — keep heavy BT files out of unit tests (`.claude/rules/fw-tests.md`).
