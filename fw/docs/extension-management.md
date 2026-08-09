# In-app extension management: list, install choice, and remote delete

Status: **implemented** — firmware #303, app #305 (phases and their review
hardening: §10). This document remains the authoritative wire-contract and
semantics reference.

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

- **No registry catalog browsing**: the app manages only the extension set the
  current firmware release ships as assets. Browsing/searching the wider
  registry (extensions from other releases, an online "store" listing with
  descriptions and screenshots) is a separate future feature.
- No runtime (no-reboot) extension unload — a delete's *slot* effects land at
  the next boot (see §3.4 for the precise pre-reboot semantics).
- No GATT surface changes of any kind (deliberate: zero handle shifts, zero
  re-pair risk, zero Android notification-slot cost).
- No firmware-initiated auto-cleanup: the firmware never deletes files on its
  own; every delete is an explicit app/user action.
- GLIM file management ships **later**, but the command group is designed for
  it now (§3.1) so adding it is new `kind` handling, not a new group.

## 3. Architecture: a custom SMP command group

### 3.1 One group, kind-parameterized (future-proof for GLIM)

New SMP group **`FILE_MGMT`, group id 64** (`MGMT_GROUP_ID_PERUSER`, the start
of the user range; nothing in-repo registers any custom group today).
Rather than an extensions-only group, every command carries a `kind` field
selecting the managed store:

| kind | directory | fence | v1 |
|---|---|---|---|
| `"ext"` | `/NAND:/ext` | `path_allowed()` (see §3.5) | ✅ implemented |
| `"glim"` | `/NAND:/glim` | same predicate, glim prefix | ❌ future (returns `KIND_UNSUPPORTED`) |

One group means one app-side client surface, one error enum, one fence
pattern; GLIM management later is a firmware-side `kind` addition plus app UI,
no new group/protocol.

Registered via `MCUMGR_HANDLER_DEFINE` + `mgmt_register_group()` from a new
`fw/src/extensions/extension_mgmt.cpp` (the `kind` dispatch keeps glim's
future home open — the file can split later), gated by
`CONFIG_APP_EXT_FILE_MANAGEMENT`.

### 3.2 LIST (command 0, read) — union of disk and boot state, paginated

The response is the **union** of (a) a live `fs_opendir`/`fs_readdir` of the
kind's directory (disk truth — sees files synced after boot) and (b) the boot
snapshot's registry slots (loaded truth — sees what is actually running).
The union is what makes every divergent state visible by name:

- on disk + loaded: the normal case;
- on disk, not loaded: uploaded since boot, or failed validation at boot;
- **loaded, not on disk: deleted (or replaced) since boot — still running.**
  This preserves (and names) the signal the old counting heuristic provided,
  and it survives app restarts because it comes from the device, not from
  screen-local state.

```
request:  { "kind": "ext", "off": 0 }
response: { "entries": [
  { "n": "demo_wave.llext",   // filename (identity key for everything)
    "disk": true,              // present in the directory right now
    "loaded": true,            // boot snapshot has it (fields below only then)
    "d": "Demo Wave",          // display name
    "s": 0,                    // slot index
    "f": false,                // faulted
    "a": true,                 // currently active
    "r": false },              // retired by a pre-reboot DELETE (see §3.4)
  ... ],
  "off": 8 }                   // continuation offset; absent = complete
```

**Pagination** is mandatory in v1 (the directory is not bounded by the
16-slot registry cap — fs_mgmt upload accepts any number of files, partial
uploads and non-`.llext` leftovers included, and the netbuf could shrink):
the handler encodes entries until the next one would overflow a conservative
response budget, then returns `off` for the client to continue from.
Ordering within one pass is FAT directory order; the app re-lists from
offset 0 after any mutation (its own delete/upload) rather than trusting
cross-call stability. The handler keeps exactly one `struct fs_dirent`
(~264 B) on the SMP workqueue stack (4096 B on proto0, raised from the
2048 default for the DELETE switch-away path — see fw/docs/threading.md;
the documented `MCUMGR_GRP_FS_DL_CHUNK_SIZE` stack incident is the
cautionary tale; one dirent is fine, arrays are not).

### 3.3 DELETE (command 1, write)

```
request:  { "kind": "ext", "name": "hello.llext" }
response: {}  |  err { group: 64, rc: <FileMgmtError> }
```

- The handler builds `<dir>/<name>` and validates it with the same pure
  predicate the upload fence uses (§3.5) — rejects `..` components, foreign
  prefixes, extra separators. The custom group does NOT go through the
  `MGMT_EVT_OP_FS_MGMT_FILE_ACCESS` hook (that event is emitted only from
  inside fs_mgmt), so calling the predicate directly is the fence.
- `fs_unlink()` on FAT via the standard fs API; SMP handlers run on the
  preemptible `smp_work_queue` (priority 3), so this respects the
  no-flash-I/O-from-cooperative-threads rule — same context fs_mgmt's own
  upload writes already use.
- If the filename maps to a boot-snapshot slot, the handler additionally
  **retires the slot** and **cleans its persisted settings** (§3.4, §4.2).
- Error enum (`FileMgmtError`, reported in the standard `err {group, rc}`
  map): `OK=0, KIND_UNSUPPORTED, INVALID_NAME, NOT_FOUND, UNLINK_FAILED,
  CLEANUP_FAILED` (the last is warning-grade: the file IS gone).

### 3.4 Pre-reboot semantics: retire the slot, honestly

The original draft claimed deletes were invisible until reboot. **That is
wrong**: `extension_host` is load-on-activate — `runtime_load()` re-opens the
`.llext` from FAT on *every* activation. After a bare unlink, the extension
keeps rendering only until it is deactivated; any re-activation (an app
toggle, `ext select`, or a shuffle hop into the slot) would fail its FAT read
and fault the slot.

So DELETE, when the name matches a boot slot, explicitly **retires** it:

- if it is the currently-active animation, the host switches away first
  (same path as a fault's fallback, without the fault banner);
- the slot is marked retired: activation requests are rejected (existing
  rejection path, distinct rc from FAULTED) and shuffle skips it (same
  eligibility check that already excludes faulted slots);
- its Is Active characteristic notifies false if it was on (existing
  machinery);
- LIST reports it as `loaded && !disk && retired` until reboot.

The slot itself (and its GATT service) still exists until the next boot —
slot numbering never changes mid-boot. Cost: a `retired` flag + one guard in
the activation path + one in shuffle eligibility, all alongside the existing
FAULTED handling.

### 3.5 The fence must not depend on the FS group

`path_allowed()` currently lives in `extension_file_transfer.cpp`, compiled
only under `CONFIG_APP_EXT_FILE_TRANSFER`, which `depends on ... &&
MCUMGR_GRP_FS` — so the naïve "new group `depends on APP_EXTENSION_HOST`"
would drop the traversal fence from the link in valid configs. The predicate
(pure, Zephyr-free, already covered by the `extensions.file_transfer`
native_sim suite) moves to its own translation unit
(`fw/src/extensions/extension_path.{h,cpp}`), compiled under
`APP_EXTENSION_HOST`, consumed by both the FS-hook fence and the new group.
Kconfig: `APP_EXT_FILE_MANAGEMENT` `depends on APP_EXTENSION_HOST &&
MCUMGR` (transport only; the FS group is NOT required).

## 4. Firmware fixes bundled with this feature

Both are pre-existing quirks that DELETE turns from theoretical into routine:

### 4.1 Last-active persistence: a separate name key for extensions

> **SUPERSEDED (issue #311).** Both keys described below, and the boot restore
> they fed, were removed. Persisting the active animation cost 850-1500 ms of
> NVS work on **every** switch — the sibling key is either deleted-when-absent or
> first-written-when-new, and both are `settings_nvs_save()` misses, which walk
> every name id. It also spent flash endurance on a per-interaction event. The
> device now always boots to the default animation, and an explicit "all
> animations off" does not survive a power cycle either.
>
> The section is kept because it explains why the design looked like this, and
> because the `delete_value()` / unregister work in §4.2 is still live and still
> used by the DELETE path. Do not re-add a write on the switch path.

`core/last_active_animation` persists the raw animation id as 4 bytes, and
the loader accepts a record only when `len == 4` — so overloading the same
key with a name string would collide (a 3-character name is exactly 4 bytes
with NUL). Instead: a **new key `core/last_active_extension`** holds the
extension's *file name* when the active animation is an extension slot;
`core/last_active_animation` keeps the id for built-ins. Writer: on switch,
write one key and clear the other (via §4.2's delete). Restore order: if the
extension key exists, resolve by filename against the boot registry (the same
name-not-index pattern the glim player established); else the id key; a name
that no longer resolves falls back exactly like today's unregistered-id path
(ZigZag). Slot renumbering after a delete then changes nothing about which
extension auto-starts.

### 4.2 `persistent_value_registry` unregister + `delete_value()`

Two gaps, both needed for DELETE's cleanup to actually stick:

- `persistent_value_store::delete_value()` wrapping `settings_delete()` (the
  store has only save/load today).
- **`persistent_value_registry_unregister(entry)`** — without it, cleanup is
  silently undone: the deleted extension's registry entries stay linked, so
  the next debounced `request_save()` flush (any brightness drag) re-saves
  the very records `settings_delete()` just removed. DELETE unregisters the
  slot's two entries, cancels their pending-save state, then deletes the
  records. Concurrency: registration is boot-time single-threaded today, but
  unregister runs on the SMP workqueue while flushes run on the settings
  workqueue — the unregister+delete is therefore **executed on the settings
  workqueue** (posted from the handler, completion-waited) so it serializes
  with any in-flight flush instead of racing the list walk.
- **Collision guard**: settings keys are display-name-derived, and two files
  can declare the same display name — `register_slot_persistence()` already
  tracks who actually owns the key (`persistRegistered`; the collision loser
  registers nothing). Cleanup runs **only when the deleted file's slot owns
  the registration**, so deleting a collision loser never touches the
  surviving extension's data. (Deleting the winner removes the shared-name
  records; the surviving loser re-registers fresh defaults at next boot —
  acceptable, and the app's identity model keys on filename throughout.)

## 5. App: SMP client additions

`McuMgrClient` is already generic over group/command; additions follow the
`getFileSha256` model:

- `SmpGroup.FILE_MGMT = 64`, a `FileMgmtCmd` enum, a `FileMgmtError` enum.
- `listDeviceFiles(kind, ...)` (paginated LIST, auto-following `off`).
- `deleteDeviceFile(kind, name)` (DELETE).
- **New** `closeOpenedFile()` — the FS-group `OPENED_FILE` (cmd 4) command
  exists in firmware fs_mgmt, but the app currently has only the enum
  constant and **no method or call site**; this is new work, not existing
  plumbing. `deleteDeviceFile` always calls it first: FatFs is compiled
  without file locking (`FF_FS_LOCK=0`) and fs_mgmt holds upload handles open
  for an idle window, so a delete racing a lingering handle could corrupt the
  FAT. Close-first plus the client's fully-serialized request chain closes
  that window from this app instance; the residual (another SMP client
  mid-upload) is accepted and documented.
- Graceful degradation: firmware without the group answers with a group-less
  SMP error — the app hides all management affordances (same pattern as the
  existing "extension check unavailable" state) rather than showing buttons
  that always fail.

## 6. App: management UI

Product direction: **one management surface, always accessible; no bulk
"install everything" anywhere** — install is always a user choice.

The existing `app/firmware-update/extensions.tsx` screen (already linked from
the landing page and the guided flow, already inside the
`McuMgrClientProvider` route group) becomes that surface:

- **Section "From this release"** — one row per release `.llext`, with
  device state from LIST + digest comparison: Installed & up to date /
  Update available / Not installed. Per-row actions: Install / Update /
  Remove. Per-row install reuses `syncExtensions()` with a single-entry
  array — no service refactor.
- **Section "Not in this release"** — LIST-union rows with no matching
  release asset, by filename (+ display name when loaded), visually
  highlighted, each with Remove. Divergent states render from LIST's flags:
  `disk && !loaded` → "takes effect after restart";
  `loaded && !disk` → "removed — restart to free the slot" (persistent,
  device-derived, survives app restarts). This is where every provisioned
  board's `hello.llext` finally becomes visible and removable.
- Remove uses the house promisified `Alert.alert` confirm with
  `style: 'destructive'`; operations claim the route group's `isBusy`.
- **Reboot from the page**: after any mutation (install/update/remove), the
  screen offers a "Restart glasses now" button using the OS-group SMP reset
  the update flow already uses — extension changes are boot-scoped, so the
  natural loop is manage → restart → re-list.
- **Guided-flow integration**: the update flow no longer bulk-syncs. At its
  existing "do all filesystem work, then reboot once" seam it surfaces the
  picker: **updates for already-installed extensions preselected**,
  not-installed release extensions unselected (install stays a choice),
  retired/not-in-release files highlighted with removal suggested
  (per-extension checkboxes, nothing silent). The one activating restart
  then covers firmware + all chosen extension changes.

**Uninstall sticks by construction**: with no bulk install, nothing ever
re-installs an extension the user removed — the next update simply shows it
as "Not installed" again. No persisted exclusion set, no per-device
preference bookkeeping, correct across multiple devices by default.

### LIST replaces the counting heuristic

`countUnmanagedExtensions()` and its "both inputs must describe the same
boot" fragility (the `unmanagedComputedRef` latch) are deleted in favor of
LIST's named, union-true answer — including the loaded-but-file-gone state
the count could only imply. The corresponding caveat paragraphs in
`app/CLAUDE.md` and the code comments retire with them.

## 7. Hazards and their mitigations (summary)

| Hazard | Mitigation |
|---|---|
| Delete is NOT boot-invisible (load-on-activate re-reads FAT) | DELETE retires the matching slot: switch-away if active, activation rejected, shuffle skips (§3.4) |
| FatFs `FF_FS_LOCK=0`: delete vs lingering fs_mgmt upload handle | New `closeOpenedFile()` called first + serialized request chain; residual cross-client race accepted & documented |
| Slot/animation-id renumbering after delete | Last-active moves to a separate name key (§4.1) — no discriminator overload of the id key |
| Settings cleanup undone by live registry entry | `persistent_value_registry_unregister()` + workqueue-serialized cleanup (§4.2) |
| Display-name collisions | Cleanup only when the deleted slot owns the registration; all identity keys on **filename** (§4.2) |
| Unbounded directory | LIST pagination (§3.2) |
| Bulk sync resurrecting uninstalled extensions | Bulk install no longer exists (§6) |
| Old firmware without group 64 | Group-less-error detection hides management UI |
| SMP workqueue stack (4096 B on proto0; invariant in fw/docs/threading.md) | One `fs_dirent` at a time in LIST; response budget drives pagination; delete's switch-away path measured 1,672 B peak |

## 8. Compatibility

- **No GATT change** — no handle shifts, no re-pairing, no Service Changed
  concerns, no Android notification-slot cost. The release notes' §4a check
  will be a genuine "GATT layout unchanged".
- Old app + new firmware: inert (nothing calls group 64).
- New app + old firmware: management hidden; per-row install still works
  (upload is plain fs_mgmt), remove/list hidden.
- The SMP group id (64), the `kind` field, and the CBOR schema become an
  app↔firmware compatibility surface — append-only, documented in this file.
- Behavior change to note in release/app notes: updates no longer
  auto-install every release extension; already-installed ones still get
  update-preselection in the flow.

## 9. Testing

- **Firmware**: the fence keeps its native_sim coverage (predicate moves to
  `extension_path.{h,cpp}`; suite follows it). New native_sim coverage:
  LIST's union/pagination logic and DELETE's retire/cleanup decision logic,
  extracted as pure functions where practical;
  `persistent_value_registry_unregister` + `delete_value` via the existing
  settings test seam. Handler wiring is exercised on-device.
- **App**: jest per house style — client methods (prototype-spy), the
  picker/plan model incl. preselection rules, the no-group degradation, the
  BLE-read-count regression guard.
- **On-device (mandatory — this is a device↔app change)**: full `/submit-pr`
  §5 gate incl. §5a OTA revalidation (confirm the monitor-count budget and a
  real firmware update end-to-end), plus the feature loop: LIST names the
  stale `hello.llext` → Remove (incl. removing the *active* extension:
  verify switch-away, shuffle skip, activation rejection) → reboot → slot
  freed, settings records gone (and not resurrected by a brightness drag
  before the reboot), last-active restored correctly by name. This session
  is also where every dev board finally gets its hello cleanup — through the
  product path instead of USB.

## 10. Implementation phases

**PR 1 — firmware** (✅ merged as #303): `extension_path.{h,cpp}` extraction;
`extension_mgmt.cpp` (group 64, kind dispatch, paginated LIST union, DELETE +
retire); `persistent_value_registry_unregister()` + `delete_value()` +
workqueue-serialized cleanup; name-key last-active persistence; Kconfig;
native_sim tests. Hardware-verified end to end, including deleting the active
extension and the boot-restore-by-name across slot renumbering. Review
hardening landed in the same PR: retire-first + host-lock-quiesced unlink
(the FF_FS_LOCK=0 corruption race), faulted-slot switch-away, async settings
purge, case-insensitive slot lookup, LFN-sized wire names, and a 4096-byte
SMP workqueue stack (measured 1,672 B peak on the deepest delete path).

**PR 2 — app** (✅ merged as #305): SMP client group + methods (incl.
`closeOpenedFile`), the management screen (sections, per-row actions, reboot
button), guided-flow picker replacing bulk sync, jest suites, `/validate-app`
+ on-device verification. Review hardening: release-unknown ≠ release-empty
(no removal suggestions after a failed GitHub lookup), picker gated on a
successful check, case-insensitive joins, idempotent delete.

**PR 3 — docs/skills** (this change): `fw/CLAUDE.md` (new group, retire
semantics, replacing the count-not-name paragraphs), `app/CLAUDE.md` (same +
no-bulk-install), `/add-extension` + `/debug-ble` touch-ups, release-skill
note about the behavior change, and the SMP-stack invariant recorded in
`fw/docs/threading.md`. `/provision-device` needs no procedural change — it
deliberately (re)installs the in-repo dev extensions (`hello`, `cpptest`) on
dev boards, which is provisioning's job, not a regression of this feature;
its skill now says so explicitly so a fresh `hello.llext` after provisioning
is read as expected, not as the cleanup failing.

**Then**: a release (the first whose update flow can clean up hello
everywhere). The PR 1/2 hardware sessions already ran the real-world proof:
the dev board's stale `hello.llext` was listed, named and removed over BLE
through the product path.
