---
paths:
  - "fw/src/extensions/extension_file_transfer.*"
  - "fw/src/extensions/extension_mgmt.*"
  - "fw/src/extensions/extension_registry.*"
  - "fw/tests/extensions/file_transfer/**"
  - "fw/docs/extension-management.md"
  - "app/services/extension-sync.ts"
  - "app/services/extension-management.ts"
  - "app/services/mcumgr.ts"
  - "app/hooks/use-extension-management.ts"
  - "app/app/firmware-update/extensions.tsx"
---

# Extension file management (MCUmgr FS group + FILE_MGMT group 64, fw ↔ app)

Loads when you read either side of extension install/list/delete. Design doc:
`fw/docs/extension-management.md`. Animation extensions are `.llext` files the firmware reads from
`/NAND:/ext` **once at boot** (`extension_registry::init()`), so any sync takes effect only after a
reboot. They ship as bare assets on the same GitHub release as the firmware zip.

## File management (group 8), fenced to `/NAND:/ext`

`CONFIG_MCUMGR_GRP_FS=y` on proto0, so the companion app can sync extensions during an OTA update:
it asks for each `.llext` file's SHA256, compares it against the digest GitHub reports for that
release asset, and re-uploads the ones that differ (`app/services/extension-sync.ts`). Adds
~4.9 KB FLASH / ~832 B RAM to the appcore. Three non-obvious things, all learned the hard way:

- **Enabling the group alone hands a bonded peer read+write access to the entire FAT disk**,
  including `/NAND:/mcuboot.bin` (the bootloader updater's staging image). `CONFIG_APP_EXT_FILE_TRANSFER`
  (`fw/src/extensions/extension_file_transfer.cpp`) registers an `MGMT_EVT_OP_FS_MGMT_FILE_ACCESS`
  callback that rejects every operation — read, write, status, hash — outside
  `extension_registry::kDirectory`. Zephyr prints a CMake WARNING when the group is on without an
  access hook; that warning is the intended alarm, don't silence it by other means. The decision is
  the pure predicate `extension_file_transfer::path_allowed()` (suite `extensions.file_transfer`).
  **A prefix check is not sufficient**: FATFS resolves `/NAND:/ext/../mcuboot.bin` straight out of
  the fenced directory, so `..` components are rejected over the whole path.
- **`CONFIG_MCUMGR_GRP_FS_HASH_SHA256` cannot be set from a `.conf` on this build, and setting it
  there fails silently.** It `depends on BUILD_WITH_TFM || MBEDTLS_SHA256`, and `MBEDTLS_SHA256` is
  declared inside `if !(NRF_SECURITY || NORDIC_SECURITY_BACKEND)` in
  `zephyr/modules/mbedtls/Kconfig.mbedtls` — unreachable, because this build uses nRF Security.
  Assigning either symbol in a `.conf` is ignored with no error; the only way to notice is that it
  never appears in `autoconf.h`. The dependency is stale rather than real:
  `fs_mgmt_hash_checksum_sha256.c` picks its backend off `CONFIG_MBEDTLS_PSA_CRYPTO_CLIENT`, which
  nRF Security does set. It is therefore force-enabled by an override in `fw/Kconfig` (a second,
  prompt-less definition with `default y if APP_EXT_FILE_TRANSFER`).
- **Do NOT "work around" that by registering a SHA256 group at runtime** via the public
  `fs_mgmt_hash_checksum_register_group()`. It compiles, looks clean, and smashes the stack:
  `fs_mgmt.c` hashes into `char output[MCUMGR_GRP_FS_CHECKSUM_HASH_LARGEST_OUTPUT_SIZE]`, sized
  **from Kconfig alone** — 4 bytes when only CRC32 is enabled — while a registered SHA256 group
  writes 32. That macro is only 32 when `CONFIG_MCUMGR_GRP_FS_HASH_SHA256` is set, so the symbol is
  what makes SHA256 *safe*, not merely available.

## FILE_MGMT group (64, `MGMT_GROUP_ID_PERUSER`) — list and delete

The FS group has no delete or directory-listing command (IDs are only 0 `FILE`, 1 `STAT`, 2
`HASH_CHECKSUM`, 3 `SUPPORTED_HASH_CHECKSUM`, 4 `OPENED_FILE`), so listing and removal live in this
firmware's own group, `fw/src/extensions/extension_mgmt.{h,cpp}` (PR #303):

- **LIST** (cmd 0, read, paginated) returns the union of the fenced directory's disk contents and
  the boot slot registry by FILE name, so "uploaded since boot" and "deleted since boot but still
  loaded" are first-class states the app renders directly. **One deliberate exception to the
  join**: a disk file whose matching slot is RETIRED is emitted *un-joined* (`loaded: false`, no
  slot annotations) — a remove-then-reinstall of the same name reads as a fresh "takes effect
  after restart" file, not as "removed". Don't write app state or a union test asserting every
  slot-matched disk file comes back `loaded: true`.
- **DELETE** (cmd 1, write) is retire-first + quiesced: the boot slot is retired (activation
  rejected until restart, shuffle skips it), the unlink runs under the host lock so an in-flight
  llext load finishes before clusters are freed (FatFs is `FF_FS_LOCK=0` — an unsynchronized unlink
  SUCCEEDS against an open file and corrupts the volume), then the display switches away if the
  file backed the current animation (healthy OR fault banner) and its persisted settings are purged
  asynchronously. A failed unlink un-retires: a failed delete is a true no-op.
- The wire enums in `extension_mgmt.h` are an app↔firmware compatibility surface
  (`app/services/mcumgr.ts` mirrors them) — **append-only**, kind-parameterized (`"ext"` today,
  `"glim"` reserved).
- DELETE runs the full animation-switch path on the SMP workqueue thread — its stack is 4096 on
  proto0 for exactly that (see the board conf comment); don't shrink it back.

## App side (`app/services/extension-sync.ts` + `app/services/extension-management.ts`)

Two layers, split deliberately:

- **extension-sync.ts** is the transfer machinery: digest comparison of every release `.llext`
  against the device's copies (`planExtensionSync`), and the download-verify-upload pipeline
  (`syncExtensions`) over MCUmgr's FS group.
- **extension-management.ts** is the product surface's pure model (PR #305, design §6): it joins the
  release plan with the firmware's FILE_MGMT LIST into two sections — "From this release" (per-row
  Install/Update/Repair/Remove) and "Not in this release" (LIST-named files, removable) — plus the
  guided flow's per-extension picker rules: updates/repairs of extensions the user already has come
  **preselected**, installs never, not-in-release files are highlighted with removal suggested but
  never pre-ticked. **There is no bulk "install everything" anywhere** — install is always a
  per-extension user choice, which is also what makes an uninstall stick. Two safety invariants,
  both regression-tested: the name join is **case-insensitive** (FatFs semantics — an exact-case
  join reported one file as both installed and junk), and an empty release-asset list with
  `releaseKnown === false` means **unknown, never "ships nothing"** — no removal is ever suggested
  against a failed GitHub lookup.

Before touching the transfer layer:

- **GitHub already publishes the hash.** `GET /releases` returns `digest: "sha256:<hex>"` per
  asset, so no sidecar manifest and no extra request. `digest` is **optional** on `GitHubAsset`
  (older releases lack it); `parseAssetSha256` returns null then, and `planExtensionSync` treats
  that as **up-to-date, not outdated** — guessing "differs" would re-upload every extension on every
  update check.
- **Asset name maps 1:1 to the device path** (`plasma.llext` → `/NAND:/ext/plasma.llext`), because
  `extension_registry::full_path()` is just `"<dir>/<name>"`. The firmware fence above is the
  boundary; `isValidExtensionAssetName` is defence in depth.
- **`uploadFile` repeats `name` in every packet** (unlike `uploadImage`, whose `image`/`sha` fields
  are first-packet only), so the chunk budget reserves `64 + name.length`. Forgetting the name's
  length silently pushes packets past the MTU for long paths.
- **"File not found" is a normal outcome, not an error** — it means "install this". `getFileSha256`
  returns null for it and rethrows everything else, keyed off the typed `SmpCommandError` (`group` +
  `rc`) rather than string-matching. `isSmpGroupError` deliberately does **not** match the legacy
  bare-`rc` shape: that carries no group, so FS "file not found" (3) would be indistinguishable
  from the generic mcumgr `EINVAL` (3).
- **Unmanaged extensions are named by FILE_MGMT, not inferred.** `McuMgrClient.listDeviceFiles()`
  returns the LIST union above, and `deleteDeviceFile()` removes one (closing any lingering fs_mgmt
  handle first; the device retires the matching slot). The old heuristic (count extension services,
  subtract release files the device could hash) is deleted along with
  `countDeviceExtensions`/`countUnmanagedExtensions`; on firmware without group 64 (group-less `rc`
  error) the app hides list/remove affordances rather than guessing.
- **Chosen extension changes are applied before the reboot, while the old firmware is still
  running** (`handleRestart`, `.claude/rules/app-firmware-update.md`). If the update is then
  abandoned, the old firmware finds newer-ABI extensions and rejects them at load (`scan_slot()`
  returns false, slot skipped) — degrades to "extension missing", never a boot failure. The guided
  flow applies the picker's ticked rows with `{skipRefresh: true}` per item and no intermediate
  re-plan (a per-item re-plan is a full hash sweep + LIST over serialized SMP — O(N²) round trips).
- **Behavior change to carry into release/app notes** (design §8): firmware updates no longer
  auto-install every release extension — already-installed ones get update-preselection in the
  picker, new ones are an explicit choice.
