---
paths:
  - "fw/src/settings/**"
  - "fw/src/bluetooth/persistent_characteristic.h"
  - "fw/src/extensions/extension_param_persistence.*"
  - "fw/src/core_config.cpp"
  - "fw/src/storage/appcfg_erase.*"
  - "fw/src/factory_reset/**"
  - "fw/src/bluetooth/animation_adapters/glim_player_animation_bt.cpp"
  - "fw/tests/settings/**"
  - "fw/tests/factory_reset/**"
  - "fw/prj.conf"
---

# Settings-backed config persistence

Loads when you read the settings/persistence code. The two coding rules this backs up live in
`fw/CLAUDE.md` "Coding rules": never persist on a per-interaction path, and budget for the cost
of a settings-key miss.

## Settings-backed config persistence

Every BT-settable config value (core config, animation parameters/strings/colors, glim
selection/loop mode, extension params) persists across power cycles via Zephyr's settings
subsystem — **except the currently-active animation, which is deliberately NOT persisted**
(issue #311; see below and the note at the top of `fw/src/pattern_controller.cpp`'s anonymous
namespace). The storage backend (`CONFIG_SETTINGS`/`CONFIG_SETTINGS_NVS`/`CONFIG_NVS`, the
`settings_storage` NVS partition on external flash in
`fw/pm_static_rgb_sunglasses_proto0_nrf5340_cpuapp.yml`, and the `settings_load()` call in
`bluetooth_init()`) predates this and exists for BT bonding — config is a second consumer.

- `fw/src/settings/persistent_value_registry.{h,cpp}` — BT-free registry mapping a stable key
  string → `{target, load_fn, save_fn}`, self-populated by static-init constructors. Lets one
  shared settings subtree handler dispatch `settings_load()` callbacks to dozens of
  independently-registered values instead of one `SETTINGS_STATIC_HANDLER_DEFINE` per
  characteristic. Storage is an **intrusive `sys_slist_t` of caller-owned
  `PersistentValueRegistryEntry` records** (issue #114) — each registrant embeds the entry in its
  own long-lived object (a characteristic member, an extension `Slot` field, a file-scope static)
  and passes its address to `persistent_value_registry_register(entry*)`; the registry links it by
  pointer. Same idiom as Zephyr's own settings backend (`settings_store.c`). There is **no fixed
  capacity and no `-ENOMEM` path** — a registration can never be silently dropped. Registration
  is effectively single-threaded (static-init/boot), but `persistent_value_registry_unregister()`
  runs at runtime on the persistence workqueue (extension DELETE purge), so list operations take
  an internal spinlock; `save_all()` and `dispatch_load()` stay lock-free for the reasons in the
  comment at the top of the `.cpp`.
- The registry list is constant-initialized (`SYS_SLIST_STATIC_INIT`), which is what lets the
  static-init constructors register before any `SYS_INIT` runs — see `fw/CLAUDE.md` "SYS_INIT
  ordering for early registration".
- `fw/src/settings/persistent_value_store.{h,cpp}` — owns the single
  `SETTINGS_STATIC_HANDLER_DEFINE("appcfg", ...)` handler (forwards to the registry's dispatch) and
  a shared debounced `k_work_delayable`. `request_save()` (re)schedules a flush of every
  registered value `CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS` after the last call, coalescing rapid
  writes (typing a string, dragging a color picker) into one flash write. **Don't reuse
  `CONFIG_BT_SETTINGS_DELAYED_STORE_MS` for this or anything else BT-free** — this module has its
  own Kconfig symbol so it has no dependency on the Bluetooth stack.
- `fw/src/bluetooth/persistent_characteristic.h` — the generic mixin; see
  `.claude/rules/fw-bluetooth-gatt.md`.
- **Bespoke persistence — glim:** `glim_player_animation_bt.cpp` persists the glim selection by
  **file name**, not index (`glim_registry`'s enumeration order can shift between boots). Since
  `glim_registry::init()` runs after `settings_load()`, the loaded name is resolved to an index
  later, in `glim_player_animation_bind_default_bt_dependencies()`.
- **Bespoke persistence — extensions** (`fw/src/extensions/extension_param_persistence.{h,cpp}` +
  `fw/src/extensions/extension_host.cpp`, issue #90 follow-up): one combined `Blob` (every scalar +
  string param value) per extension, registered under key `"ext/<sanitized displayName>"` (never
  slot index, since `/NAND:/ext/` file sets can shift between boots), plus a second per-slot key
  for the shuffle-include flag (issue #243). An extension's identity is only known from its
  manifest, discovered strictly *after* `bluetooth_init()`'s boot-wide `settings_load()` has run,
  so the automatic replay can never find these keys. `init()` therefore calls
  `register_slot_persistence()` for each slot only after its BLE service fully registered (so a
  rolled-back slot never leaves a dangling entry), then runs ONE `settings_load_subtree("appcfg/ext")`
  for all of them. A display-name collision (`-EEXIST`) leaves that slot unpersisted rather than
  clobbering the other's key. Saves reuse `persistent_value_registry_mark_dirty()` +
  `request_save()`, hooked into `extension_host::setParamValue()`/`writeParamString()` exactly like
  `BtGattPersistentCharacteristic::onWrite()`. **A faulted extension has its persisted params
  cleared**: `sandbox_fault()` resets `paramValues`/`stringValues` to manifest defaults and
  synchronously (not via the debounce) overwrites the persisted blob, since a bad persisted value
  could be what caused the crash — without this, both an `ext select` retry and a reboot would
  reproduce the crash from the same poisoned value (which faults clear params:
  `.claude/rules/fw-extensions.md`).
- **`CONFIG_APP_PERSIST_BT_CONFIG`** (default `y`) gates the whole feature: call sites are wrapped
  in `if constexpr (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG))` (in the template mixin) or
  `if (IS_ENABLED(...))` (plain `.cpp`), so doLoad/doSave compile out when disabled. It exists
  because the legacy DK board's internal-flash slot had no room for it (DK now lives on the
  `dk-support` branch); it is kept on `main` for any future flash-tight board, rather than
  `#ifdef`-ing every persistent characteristic declaration.

## No persistence on per-interaction paths (issue #311)

Never persist a value on a path that fires once per user action (an animation switch, a button
press, a shuffle hop) rather than once per deliberate setting change. Flash endurance is a
first-class budget: the NAND has finite erase/write cycles and they were measurably being burned.
`pattern_controller.cpp` **used to** persist a single "last active animation" key hooked into
`pattern_controller_change_to_animation()`; issue #311 removed it — it cost 850-1500 ms of NVS
work per switch and burned endurance on a per-interaction event. The device now always boots to
the default animation, and an explicit "all animations off" (`Animation::None`) does not survive a
power cycle either. That trade was made on purpose; do not re-add a write on that path. A settings
write is for a value the user asked to keep, not for tracking state.

## A settings-key MISS is orders of magnitude more expensive than a hit

`settings_nvs_save()` resolves a name by walking every name id with an `nvs_read()` per id; a hit
exits early, a miss (first write of a new key, or a delete of a key that is not present) runs the
walk to completion. Measured on proto0 with 19 keys resident: **1-15 ms for a hit, 850-1500 ms for
a miss.** `CONFIG_SETTINGS_NVS_NAME_CACHE=y` (in `fw/prj.conf`) removes the miss walk — but only
while the cache is not overflowed, and **overflow is silent**: `cache_total >
CONFIG_SETTINGS_NVS_NAME_CACHE_SIZE` just reverts to the slow path with no warning.

**`settings list` cannot detect that overflow** — it reports RESIDENT keys, and the counter that
overflows is cumulative: `cache_total` is set to the resident count at load and thereafter only
ever incremented, once per first-write of a new name; the delete branch returns before any cache
bookkeeping, so deleting a key never frees its slot. A device re-paired a few times with
extensions installed and removed can be past the cache size while `settings list` still shows ~20
keys. Size it against CHURN — the budget and the `settings_nvs.c` line references live next to
`CONFIG_SETTINGS_NVS_NAME_CACHE_SIZE` in `fw/prj.conf`, and the value actually flashed is in
`fw/build/fw/zephyr/include/generated/zephyr/autoconf.h` (a board fragment can override
`prj.conf`). No size is quoted here on purpose: an earlier version of this note carried one that
was wrong from the day it was written — introduced in `df326c42` alongside the `prj.conf` line it
contradicted, so it never drifted, it was simply never checked against the tree.
