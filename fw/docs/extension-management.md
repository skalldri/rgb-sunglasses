# In-app extension management: list, install choice, and remote delete

Status: **design** — approved direction, not yet implemented. Implementation is
phased (§10).

## 1. Motivation

Extensions can be installed over BLE but never removed: MCUmgr's FS group has
no delete and no directory listing, so cleanup is a USB-mass-storage job. That
gap now has a concrete cost — `hello.llext` shipped on every provisioned
device before fw-v3.1.1 retired it from releases, and it sits on all of them
consuming one of the 16 extension slots with no remote cleanup path. More
broadly, as the community registry grows, "the app installs every registry
extension on every device" stops scaling: users need to see what's on their
glasses by name, choose what to install, remove what they don't want, and be
helped to clean up extensions no release ships anymore.

Design inputs worth naming:

- **The count-not-name limitation is not fundamental.** The app today can only
  *count* unmanaged extensions (`countUnmanagedExtensions()` in
  `app/services/extension-sync.ts`) because GATT exposes each slot's manifest
  *display* name ("Hello Extension") while releases ship *file* names
  ("hello.llext") — and the firmware knows both (`extension_host` keeps each
  slot's `fileIndex` into `extension_registry`). Exposing filenames makes the
  cleanup UI precise instead of "1 extension is not part of this release".
- fs_mgmt cannot be extended from this repo: its command table is SDK code,
  and `mgmt_find_handler()` hard-breaks on out-of-range command IDs, so a
  second downstream group registered under the FS group id is unreachable.

## 2. Non-goals (v1)

- No store browsing/discovery UI beyond the current release's extension set
  (the registry is still "the release ships what it ships"; per-extension
  install *choice* is in scope, catalog browsing is not).
- No runtime (no-reboot) extension unload — deletes take effect at the next
  boot, exactly like installs today.
- No GATT surface changes of any kind (deliberate: zero handle shifts, zero
  re-pair risk, zero Android notification-slot cost).
- No firmware-initiated auto-cleanup: the firmware never deletes files on its
  own; every delete is an explicit app/user action.

## 3. Architecture: a custom SMP command group

New SMP group **`EXT_MGMT`, group id 64** (`MGMT_GROUP_ID_PERUSER`, the start
of the user range; nothing in-repo registers any custom group today), with two
commands, transported over the existing SMP characteristic the app already
uses for firmware update and file sync. Registered via
`MCUMGR_HANDLER_DEFINE` + `mgmt_register_group()` from a new
`fw/src/extensions/extension_mgmt.cpp`, gated by
`CONFIG_APP_EXT_FILE_MANAGEMENT` (`depends on APP_EXTENSION_HOST`,
`MCUMGR_GRP_FS` not required).

### LIST (command 0, read)

Live `fs_opendir`/`fs_readdir` of `/NAND:/ext` (disk truth — sees files
synced after boot, which is exactly the state the app is in right after a
sync), each entry annotated from the boot snapshot when the filename matches
a registry slot:

```
request:  {}
response: { "entries": [
  { "n": "demo_wave.llext",   // filename (disk truth)
    "l": true,                 // loaded at boot (registry has it)
    "d": "Demo Wave",          // display name  (only when l)
    "s": 0,                    // slot index    (only when l)
    "f": false,                // faulted       (only when l)
    "a": true },               // currently active (only when l)
  ... ] }
```

Sizing: 16 entries × ~50 B CBOR each fits comfortably in the 2048-byte
`CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE`; the handler keeps exactly one
`struct fs_dirent` (~264 B) on the SMP workqueue stack (2048 B — the
documented `MCUMGR_GRP_FS_DL_CHUNK_SIZE` stack incident is the cautionary
tale; one dirent is fine, arrays are not).

A disk file absent from the boot snapshot reports `l: false` (uploaded since
boot, or failed validation) — the app renders it as "takes effect after
restart" / "not loaded".

### DELETE (command 1, write)

```
request:  { "name": "hello.llext" }
response: {}  |  err { group: 64, rc: <ExtMgmtError> }
```

- The handler builds `/NAND:/ext/<name>` and validates it with the same pure
  predicate the upload fence uses (`extension_file_transfer::path_allowed()` —
  rejects `..` components, foreign prefixes, extra separators; already covered
  by the `extensions.file_transfer` native_sim suite). The custom group does
  NOT go through the `MGMT_EVT_OP_FS_MGMT_FILE_ACCESS` hook (that event is
  emitted only from inside fs_mgmt), so calling the predicate directly is the
  fence.
- `fs_unlink()` on FAT via the standard fs API; SMP handlers run on the
  preemptible `smp_work_queue` (priority 3), so this respects the
  no-flash-I/O-from-cooperative-threads rule — same context fs_mgmt's own
  upload writes already use.
- **Settings cleanup**: if the filename maps to a boot-snapshot slot, the
  handler deletes that extension's persisted records —
  `ext/<sanitized displayName>` and `ext/<...>/shuffle` — via a new
  `persistent_value_store::delete_value()` (wrapping `settings_delete()`;
  the store currently has only save/load). Only the firmware can do this:
  the app never reliably knows the display↔file mapping, and the keys are
  display-name-derived. Without it, install/uninstall cycles leak NVS records
  forever.
- Error enum (`ExtMgmtError`, reported in the standard `err {group, rc}`
  map): `OK=0, INVALID_NAME, NOT_FOUND, UNLINK_FAILED, SETTINGS_CLEANUP_FAILED`
  (the last is a warning-grade rc: the file IS gone).

### Effects are boot-scoped — by design

Nothing at runtime re-reads `/NAND:/ext` (the registry scans once at boot
from the pattern-controller thread), llext keeps no file handle open after
load (the ELF is fully copied to the llext heap), and GATT services never
unregister at runtime. So deleting any file — including the currently-active
extension's — is safe and invisible until reboot. The app already frames
extension changes as "take effect when your sunglasses restart"; delete uses
the same framing and the same reboot.

## 4. Firmware fixes bundled with this feature

Both are pre-existing quirks that DELETE turns from theoretical into routine:

1. **Persist the last-active extension by name, not raw animation id.**
   `pattern_controller` persists `core/last_active_animation` as the id;
   extension ids are `0x40 + slot`, and slots renumber alphabetically at every
   boot-time rescan — so deleting (or installing) a file that sorts before the
   active extension makes the device silently boot into a *different*
   extension. Fix mirrors the glim player's established name-based persistence:
   for extension animations, persist the extension's name and re-resolve at
   restore; built-in animations keep the id path. Restore of a name that no
   longer exists falls back exactly like today's unregistered-id path (ZigZag).
2. **`persistent_value_store::delete_value()`** as described above — a small,
   generic addition (the settings subsystem supports `settings_delete()`);
   used by DELETE's cleanup and available to future consumers.

## 5. App: SMP client additions

`McuMgrClient` is already generic over group/command; additions are small and
follow the `getFileSha256` model:

- `SmpGroup.EXT = 64`, an `ExtMgmtCmd` enum, an `ExtMgmtError` enum.
- `listExtensions(): Promise<DeviceExtension[]>` (LIST).
- `deleteExtension(name: string)` (DELETE) — **always preceded by the existing
  FS-group `OPENED_FILE` close command**: FatFs is compiled without file
  locking (`FF_FS_LOCK=0`) and fs_mgmt holds upload handles open for an idle
  window, so a delete racing a lingering handle could corrupt the FAT. Closing
  first, plus the client's fully-serialized request chain, closes that window
  from this app instance; the residual (another SMP client mid-upload) is
  accepted and documented.
- Graceful degradation: firmware without the group answers with a group-less
  SMP error — the app hides all management affordances (same pattern as the
  existing "extension check unavailable" state) rather than showing buttons
  that always fail.

## 6. App: management UI

Per the product direction: **one management surface, always accessible**, that
lists the current release's extensions with install/uninstall, highlights
extensions no release ships, and is woven into the update flow as a
suggestion.

The existing `app/firmware-update/extensions.tsx` screen (already linked from
the landing page and the guided flow's success step, already inside the
`McuMgrClientProvider` route group) grows into it:

- **Section "From this release"** — one row per release `.llext`: status
  (Up to date / Update available / Not installed / Excluded) + a per-row
  action (Install / Update / Remove). Per-row install reuses
  `syncExtensions()` with a single-entry array — no service refactor.
- **Section "Not in this release"** — LIST-derived rows for device files with
  no matching release asset, shown by filename (+ display name when loaded),
  visually highlighted, each with Remove. This is where every provisioned
  board's `hello.llext` finally becomes visible and removable.
- Remove uses the house promisified `Alert.alert` confirm with
  `style: 'destructive'`; operations claim the route group's `isBusy` like the
  flow does; rows removed this boot render as "removed — takes effect after
  restart" (LIST is disk truth, so a re-list confirms immediately).
- **Guided-flow integration**: the update flow's existing
  "do all filesystem work, then reboot once" seam (`handleRestart`) gains the
  retired-extension prompt — before the activating restart, if LIST shows
  files the release doesn't ship (and the user hasn't excluded the prompt),
  the flow *suggests* removing them (per-extension checkboxes, default
  checked for known-retired names, nothing silent). Deleting there costs no
  extra reboot.

### Uninstall must stick: the exclusion preference

Without memory, the next guided update's bulk sync would reinstall anything
the user removed. The app gains a small persisted preference — a set of
excluded extension names (per-app, not per-device, keyed by asset filename) —
stored via the app's local storage (AsyncStorage; the first persisted app
preference, kept deliberately tiny). `planExtensionSync` filters excluded
names out of the bulk path (they render as "Excluded" with a Re-include
action); explicit per-row Install always overrides and clears the exclusion.

### LIST replaces the counting heuristic

`countUnmanagedExtensions()` and its "both inputs must describe the same
boot" fragility (the `unmanagedComputedRef` latch) are deleted in favor of
LIST's named, disk-true answer. The corresponding caveat paragraphs in
`app/CLAUDE.md` and the code comments retire with them.

## 7. Hazards and their mitigations (summary)

| Hazard | Mitigation |
|---|---|
| FatFs `FF_FS_LOCK=0`: delete vs lingering fs_mgmt upload handle | App closes opened file first + serialized request chain; residual cross-client race accepted & documented |
| Slot/animation-id renumbering after delete | Last-active persistence moves to name-based (§4.1) |
| Orphaned `ext/<name>` NVS records | DELETE handler cleans them via `delete_value()` (§3) |
| Bulk sync resurrecting uninstalled extensions | Persisted exclusion set (§6) |
| Old firmware without group 64 | Group-less-error detection hides management UI |
| Display-name collisions (two files, same manifest name) | All operations key on **filename**; display name is decoration |
| SMP workqueue stack (2048 B) | One `fs_dirent` at a time in LIST; no arrays |

## 8. Compatibility

- **No GATT change** — no handle shifts, no re-pairing, no Service Changed
  concerns, no Android notification-slot cost. The release notes' §4a check
  will be a genuine "GATT layout unchanged".
- Old app + new firmware: inert (nothing calls group 64).
- New app + old firmware: management hidden, sync works as today.
- The SMP group id (64) and its CBOR schema become an app↔firmware
  compatibility surface — append-only, documented in this file.

## 9. Testing

- **Firmware**: the fence is already covered (`extensions.file_transfer`).
  New native_sim coverage: a pure name→path/validation helper for the group
  (same pattern as `path_allowed`), and `persistent_value_store::delete_value`
  via the existing settings test seam. Handler wiring is exercised on-device.
- **App**: jest per house style — service tests for `listExtensions`/
  `deleteExtension`/exclusion filtering (prototype-spy on `McuMgrClient`),
  screen tests via `renderWithMcuMgr` including the no-group degradation and
  the BLE-read-count regression guard.
- **On-device (mandatory — this is a device↔app change)**: full `/submit-pr`
  §5 gate incl. §5a OTA revalidation (the SMP surface grew: confirm the
  monitor-count budget and a real firmware update end-to-end), plus the
  feature loop: LIST names the stale `hello.llext` → Remove → reboot →
  slot freed, settings records gone, last-active restored correctly by name.
  This session is also where every dev board finally gets its hello cleanup —
  through the product path instead of USB.

## 10. Implementation phases

**PR 1 — firmware**: `extension_mgmt.cpp` (group 64, LIST + DELETE),
`persistent_value_store::delete_value()`, name-based last-active persistence,
Kconfig, native_sim tests. Full `/submit-pr` gate with on-device + app
verification (the app side of the gate uses a dev build of PR 2's client
methods, or the debug page's raw SMP path).

**PR 2 — app**: SMP client group + methods, extension-sync listing/exclusion
model, the management screen, guided-flow retired-extension prompt, jest
suites, `/validate-app`.

**PR 3 — docs/skills**: `fw/CLAUDE.md` (new group, boot-scoped semantics,
retired count-not-name paragraphs), `app/CLAUDE.md` (same), `/add-extension`
+ `/provision-device` + `/debug-ble` touch-ups, release-notes template note.

**Then**: a release (the first whose update flow can clean up hello
everywhere), and the hardware verification session doubles as the real-world
proof on your own board.
