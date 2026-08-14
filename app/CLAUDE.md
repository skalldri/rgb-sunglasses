# RGB Sunglasses App - Copilot Instructions

## Project Overview

React Native Expo app for controlling RGB sunglasses via Bluetooth Low Energy (BLE). Enables color customization, animation control, and firmware updates through a mobile interface.

## Architecture

### Core Pattern: BLE GATT Characteristic-Based State Management

The app mirrors BLE GATT characteristics as UI controls. Each characteristic on the device (boolean, uint32, string, or custom color) automatically renders as the appropriate input (Switch, TextInput, or ColorPicker).

**Critical Files:**

- [context/bluetooth-context.tsx](context/bluetooth-context.tsx) - Global BLE state with `BluetoothContextDevice` structure
- [app/(tabs)/device-state/](<app/(tabs)/device-state/>) - a **directory**, not a single file: `index.tsx` is the Controls menu (service list), `[serviceUuid].tsx` is the per-service detail screen
- [hooks/use-characteristic-editor.tsx](hooks/use-characteristic-editor.tsx) - the characteristic dispatch/decode logic: `renderCharacteristicInput()` switches on CPF format to pick a control, `decodeValueForInput()` decodes base64 values per format
- [constants/bluetooth.ts](constants/bluetooth.ts) - BLE GATT CPF format constants and UUID mappings

### Data Flow

1. **Connection**: `connect()` in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts) (triggered from [components/bluetooth-device-list-item.tsx](components/bluetooth-device-list-item.tsx)) discovers services/characteristics and reads CPF descriptors to determine data types
2. **Rendering**: `renderCharacteristicInput()` in [hooks/use-characteristic-editor.tsx](hooks/use-characteristic-editor.tsx) switches on `cpfFormat` to render appropriate control (a `Characteristic*` component from [components/](components/))
3. **Updates**: Write operations use `writeWithResponse()`, with optimistic updates reverted on error
4. **Encoding**: All BLE values are base64-encoded (`btoa`/`atob`). Numbers use little-endian byte order

### Custom Extensions

- `BLE_GATT_CPF_FORMAT_CUSTOM_COLOR` (0xE0): Non-standard format for RGB color as uint32 (lower 24 bits = color, byte 3 = color mode — see "Color Encoding" below)
- Custom service UUIDs starting with `12345678-1234-5678-000X-` for different animation services

## Key Technologies

### MCU Manager (MCUmgr)

[services/mcumgr.ts](services/mcumgr.ts) implements SMP (Simple Management Protocol) for Zephyr RTOS firmware updates:

- CBOR-encoded messages with 8-byte headers
- Chunked image uploads (max 256 bytes per write)
- Multi-stage process: upload → test → confirm → reset
- **Important**: Uses sequence numbers and fragmented responses across multiple BLE notifications

### Firmware update: what you can and cannot verify an installed image against

The guided flow (`app/firmware-update/flow.tsx` + `hooks/use-firmware-update-flow.ts`) proves an update actually landed by comparing hashes. Getting the *right* hash is the whole trick, and there are two wrong answers that both look plausible:

- **The zip manifest has no hash.** Verified against the published `fw-v2.1.0` `dfu_application_proto0.zip`: each `manifest.json` entry carries only `type`, `board`, `soc`, `load_address`, `image_index`, the two `slot_index` fields, `version`/`version_MCUBOOT`, `size`, `file`, `modtime`. Don't go looking for a digest there — and don't trust the `ManifestFile` interface as evidence either way; check a real artifact.
- **`sha256(whole .bin)` is NOT the value the device reports.** The file ends with the MCUboot TLV trailer, which contains the digest, so the digest cannot cover it. For `fw-v2.1.0`'s `fw.signed.bin` the real values are `IMAGE_TLV_SHA256 = eeacf0fa…` versus `sha256(file) = 9f5d7d3a…`. Verification built on the whole-file digest would fail every single update.

The value the device reports as a slot's `hash` in `getImageState()` **is** the image's own `IMAGE_TLV_SHA256` (type `0x10`) — hardware-confirmed: after uploading `fw-v2.1.0` the board reported `eeacf0fa…` for slot 1, matching the TLV extracted from the file. So `parseImageSha256()` (`services/mcumgr.ts`) reads that TLV out of the `.bin` and the flow verifies the post-reboot active slot against it, which is end-to-end: it never trusts the device's account of what it received. `fw/tools/dump_dfu_tlv.py` is the reference implementation and the cross-check.

Two related facts worth keeping in mind:

- **Images are staged as permanent (`setImageState(hash, true)`), and there is no rollback to be had.** The bootloader is built `CONFIG_BOOT_UPGRADE_ONLY=y` (overwrite-only), whose Kconfig help says it *"prevents the fallback recovery"*, and this SoC's architecture cannot support a swap mode. Hardware-confirmed: an image staged as `pending` came back `active confirmed` with nothing confirming it. **Do not "fix" this to `confirm=false` expecting MCUboot to revert a bad image — it cannot**, and a test-then-confirm sequence would be a permanently no-op extra step. A failed verification means the device is running the wrong firmware and needs re-flashing, not restarting.

- **Verification cannot use the same signal for both cores.** Measured on hardware with fw-v2.1.0:

  | image | file TLV | staged slot 1 | active slot 0 after install |
  |---|---|---|---|
  | 0 app core | `eeacf0fa…` | `eeacf0fa…` | `eeacf0fa…` — stable |
  | 1 net core | `e43ebfa1…` | `e43ebfa1…` | `4d4b2c28…` — changes |

  The app core's image is flashed into its own slot verbatim so its hash survives; the net-core image is a wrapper the app core unwraps over IPC, so its file TLV can never match post-install. Hashes are checked at staging for every image (proving the upload arrived intact) and after reboot for the app core only, plus a version check for both. Version comparison must stop at the `+build` boundary — a bare `startsWith` accepts a shorter running version (`'2.1.10+0'.startsWith('2.1.1')` is true), which would verify a failed update as success.

- **Sync extensions BEFORE the activating restart.** They live on the FAT disk and are read at boot, so syncing after the reboot needs a second reboot — and a device that reboots into new firmware with old-ABI extensions has them rejected by `scan_slot()`, so the animations silently vanish. The guided flow does this in `handleRestart` (`app/firmware-update/flow.tsx`); a sync failure there does not block the restart, since the images are already staged and extensions can be retried afterwards.
- **After an OTA stages an image, the first J-Link reflash boots the OTA'd image, not the one you just flashed** — MCUboot consumes the pending swap first. It takes a second flash to actually land your build. Check the boot banner's version before trusting any measurement taken after a reflash.

### GitHub-releases auto-update check (now `hooks/use-firmware-release.ts`)

On mount (once an MCUmgr client connects), the modal sequentially: (1) calls `client.getOsInfo('i')` (OS Management group, `OsCmd.INFO`, added to [services/mcumgr.ts](services/mcumgr.ts) for this feature) to read the device's board name string (e.g. `rgb_sunglasses_proto0_nrf5340_cpuapp`), (2) derives `'proto0' | 'dk' | null` from it via `extractBoardRevision()`, then (3) calls `fetchLatestRelease('skalldri', 'rgb-sunglasses')` (GitHub REST API, no auth — [services/github-releases.ts](services/github-releases.ts)) and picks the release asset whose filename contains the board revision (`findAssetForBoard()`). The found asset's tag (`vX.Y.Z` → stripped via `parseVersionFromTag()`) is compared against the currently-active image's `version` (from `getImageState()`, slot 0 + `active`) via `compareVersions()`; a strictly-older device version surfaces an "Update Available" card with a **Download Update** button. That button downloads the release zip via `expo-file-system/legacy`'s `createDownloadResumable` (not the newer `expo-file-system/next` `File` API used elsewhere in this file — `next` has no resumable-download primitive yet) straight into the same `parseFirmwarePackageFromBase64()` → `firmwarePackage` state the manual `.zip`-picker path already populates, so the existing upload/test/confirm flow (`handleStartUpdate`) is unmodified and shared by both paths.

This was originally built and verified on the pre-monorepo standalone app repo (`skalldri/rgb-sunglasses-app`, `auto-update` branch) and silently never made it across during the monorepo migration — ported back into this repo by re-deriving the diff from that branch rather than re-implementing from scratch. The GitHub release lookup is unauthenticated and rate-limited per-IP by GitHub (60 req/hr) — fine for manual on-demand checks from a single device, but don't add polling/retry-on-mount behavior without adding a token.

### Extension management ([services/extension-sync.ts](services/extension-sync.ts) + [services/extension-management.ts](services/extension-management.ts))

Animation extensions are `.llext` files the firmware reads from `/NAND:/ext` at boot. They ship as bare assets on the same GitHub release as the firmware zip. Two layers, split deliberately:

- **extension-sync.ts** is the transfer machinery: digest comparison of every release `.llext` against the device's copies (`planExtensionSync`), and the download-verify-upload pipeline (`syncExtensions`) over MCUmgr's FS group (group 8).
- **extension-management.ts** is the product surface's pure model (PR #305, design `fw/docs/extension-management.md` §6): it joins that release plan with the firmware's FILE_MGMT LIST into two sections — "From this release" (per-row Install/Update/Repair/Remove) and "Not in this release" (LIST-named files, removable) — plus the guided flow's per-extension picker rules: updates/repairs of extensions the user already has come **preselected**, installs never, not-in-release files are highlighted with removal suggested but never pre-ticked. **There is no bulk "install everything" anywhere** — install is always a per-extension user choice, which is also what makes an uninstall stick (nothing ever re-installs a removed extension). Two safety invariants encoded in the model, both regression-tested: the name join is **case-insensitive** (FatFs semantics — an exact-case join reported one file as both installed and junk), and an empty release-asset list with `releaseKnown === false` means **unknown, never "ships nothing"** — no removal is ever suggested against a failed GitHub lookup.

Points worth knowing before touching the transfer layer:

- **GitHub already publishes the hash.** `GET /releases` returns `digest: "sha256:<hex>"` per asset, so no sidecar manifest and no extra request. `digest` is **optional** on `GitHubAsset` — the field is a recent API addition and older releases lack it. `parseAssetSha256` returns null then, and `planExtensionSync` treats that as **up-to-date, not outdated**: guessing "differs" would re-upload every extension on every single update check.
- **Asset name maps 1:1 to the device path** (`plasma.llext` → `/NAND:/ext/plasma.llext`), because `extension_registry::full_path()` is just `"<dir>/<name>"`. The firmware rejects anything outside that directory regardless (see `fw/CLAUDE.md`, "File management (group 8)"), so `isValidExtensionAssetName` is defence in depth, not the boundary.
- **`uploadFile` repeats `name` in every packet** (unlike `uploadImage`, whose `image`/`sha` fields are first-packet only), so the chunk budget is `mtu - 64 - name.length`. Forgetting the name's length silently pushes packets past the MTU for long paths.
- **"File not found" is a normal outcome, not an error** — it means "install this". `getFileSha256` returns null for it and rethrows everything else, keyed off the typed `SmpCommandError` (`group` + `rc`) rather than string-matching. Note `isSmpGroupError` deliberately does **not** match the legacy bare-`rc` shape: that carries no group, so FS "file not found" (3) would be indistinguishable from the generic mcumgr `EINVAL` (3).
- **Unmanaged extensions are named by the firmware's own FILE_MGMT group (64), not inferred.** `McuMgrClient.listDeviceFiles()` returns the union of the extension directory's disk contents and the boot slot registry by FILE name — including the divergent boot-scoped states (uploaded-since-boot, deleted-but-still-loaded) — and `deleteDeviceFile()` removes one (closing any lingering fs_mgmt handle first; the device retires the matching slot). The old heuristic — count extension animation services, subtract release files the device could hash, valid only within one boot — is deleted along with `countDeviceExtensions`/`countUnmanagedExtensions`; on firmware without group 64 (group-less `rc` error) the app hides list/remove affordances rather than guessing. See `services/extension-management.ts` (plan + picker rules) and `fw/docs/extension-management.md`.
- **Chosen extension changes are applied before the reboot, while the old firmware is still running.** If the update is then abandoned, the old firmware finds newer-ABI extensions and rejects them at load (`scan_slot()` returns false, slot skipped) — degrades to "extension missing", never a boot failure. The guided flow applies the picker's ticked rows with `{skipRefresh: true}` per item and no intermediate re-plan (a per-item re-plan is a full hash sweep + LIST over serialized SMP — O(N²) round trips).
- **Behavior change to carry into release/app notes** (design §8): firmware updates no longer auto-install every release extension — already-installed ones get update-preselection in the picker, new ones are an explicit choice.

### React Native BLE PLX

Singleton `bleManager` in [hooks/ble-manager.ts](hooks/ble-manager.ts) with state restoration (connect/discovery logic lives in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts)). **Patch applied** via [patches/react-native-ble-plx+3.5.0.patch](patches/react-native-ble-plx+3.5.0.patch) - check patch file before upgrading library.

The patch's core fix: the library's Android native module (`BlePlxModule.java`) calls `promise.reject(null, errorConverter.toJs(bleError))` on BLE operation errors — `code` is `@NonNull`-annotated in Kotlin's `Promise.reject`, so passing `null` throws a secondary native `NullPointerException` that crashes the entire app process (it's a native crash, not a JS promise rejection, so no JS-level try/catch can stop it). The patch replaces `null` with `bleError.errorCode.name()` at every call site. If a future library upgrade reintroduces this pattern (`grep -n "reject(null" node_modules/react-native-ble-plx/android/.../BlePlxModule.java`), reapply the same fix and regenerate the patch (see the `patch-package` note in Known Issues & Quirks below).

### Expo Router (File-Based Routing)

- Tabs: [app/(tabs)/](<app/(tabs)/>) directory (bluetooth, device-state/ — itself a directory with `index.tsx` + `[serviceUuid].tsx`, index)
- Modals: [color-picker-modal.tsx](app/color-picker-modal.tsx), and the firmware-update group below
- **Firmware update is a nested stack**, not a single modal: `app/firmware-update/` holds `index` (landing), `flow` (the guided update), `debug` (the old high-detail page) and `extensions`, wrapped by a `_layout.tsx` that mounts `McuMgrClientProvider`. The provider is why it is a group at all — `McuMgrClient.initialize()` registers a `monitor()` on the SMP characteristic, and a pushed screen does **not** unmount the one below it, so a per-screen `useMcuMgrClient` would put two clients on one characteristic (two notification registrations against Android's 15-slot budget, two response handlers racing). Screens draw their own in-body headers so they stay renderable in unit tests without a navigator.
- Query params for modal communication: `charUuid`, `r`, `g`, `b`, `mode`, `speed` (color picker; `mode`/`speed` added for the issue #259 color modes)

## Development Workflow

### Running the App

```bash
npx expo start
# Then press 'a' for Android or 'i' for iOS
```

### iOS (macOS) builds

iOS cannot be built in the Linux devcontainer (needs macOS/Xcode) — build it on a Mac (e.g. the
Mac Mini M1). One-time setup: `app/scripts/macos-setup.sh` (idempotent; installs Xcode CLT check +
Homebrew packages, runs `npm ci`, then `expo prebuild --platform ios`). Then `npm run ios`.

The app is a managed Expo project, so `ios/` is generated by `expo prebuild` and gitignored. The
generated Xcode **workspace and scheme are both `RGBGlasses`** (derived from the Expo `name`
"RGB Glasses" via `@expo/config-plugins`' `sanitizedName()`, which strips spaces), **not** the
slug/scheme `rgbsunglassesapp`. **Renaming `expo.name` renames the workspace**, so
`app-ios-ci.yml` and `app-release.yml` both hardcode this string and have to change with it. The iOS `bundleIdentifier` is
`com.autom8ed.rgbsunglassesapp` (same string as the Android package). CI builds the unsigned
simulator binary on a self-hosted macOS runner via `.github/workflows/app-ios-ci.yml` with
`CODE_SIGNING_ALLOWED=NO` — no Apple Developer account or signing secrets. That workflow triggers
**only on `push` + `workflow_dispatch`, never `pull_request`**: the repo is public and a fork PR runs
the fork's own copy of the workflow, so an in-file `if:` guard is not a security boundary — push-only
means fork code can never reach the self-hosted Mac (forks can't push or dispatch). Per-PR `test` +
Android coverage stays in `app-ci.yml` on GitHub-hosted runners.

**BLE does not work on the iOS Simulator** — the simulator has no Bluetooth radio, so scanning
finds nothing. Simulator verification covers build + UI + navigation only; any live BLE round-trip
(scan/connect/control/firmware-update) needs a **physical iPhone**. The Android-only BLE tuning
calls (`requestConnectionPriority`, `refreshGatt`, `requestMTU`) are already no-ops/try-caught on
iOS, and `requestPermissions()` returns `true` on iOS (BLE permission strings come from the
`react-native-ble-plx` Expo plugin config in `app.json`, which also sets `UIBackgroundModes`).

**TestFlight releases**: the `ios-testflight` job in `.github/workflows/app-release.yml` builds a
signed Release archive and uploads it to TestFlight. The job is gated on the
`TESTFLIGHT_PUBLISH_ENABLED` repo variable — it must be `true` or the job is skipped (a pause
switch that avoids editing the workflow). It runs on the same self-hosted Mac runner — which runs
under a dedicated `ci-runner` macOS account with Automatic Login, not a developer's own login
session, so the job's throwaway signing keychain never shares a search list with anyone's
interactive session (see `app/README.md`'s "Runs under a dedicated ci-runner account" section) —
triggered by `app-vX.Y.Z` tags (in parallel with the Android release job — neither gates the other)
or by `workflow_dispatch` with explicit `version` + `build_number` inputs (iOS-only; the Android
job is skipped — used for the first upload and pipeline validation). Signing: automatic signing +
`xcodebuild -allowProvisioningUpdates` authenticated by an App Store Connect API key (secrets
`ASC_API_KEY_P8` base64 / `ASC_KEY_ID` / `ASC_ISSUER_ID`; the non-sensitive Team ID is the repo
*variable* `APPLE_TEAM_ID`) manages the *provisioning profile* — but the ASC key does NOT sign. The
signing **certificates + private keys** are imported at build time from two secret pairs —
`APPLE_DEV_CERT_P12` / `APPLE_DEV_CERT_PASSWORD` (Apple Development) and `APPLE_DIST_CERT_P12` /
`APPLE_DIST_CERT_PASSWORD` (Apple Distribution), each a base64 `.p12` — into a throwaway keychain the
`Set up signing keychain` step creates, unlocks, and `set-key-partition-list`s inside the job's own
session (deleted in the always() cleanup). **Both identities are required** (two separate `.p12`s
because Keychain Access / Xcode won't export both into one file): automatic signing archives with the
**Development** identity (Xcode's
default — the Expo project sets only `DEVELOPMENT_TEAM`, no explicit `CODE_SIGN_IDENTITY`), then
`-exportArchive` re-signs for app-store with the **Distribution** identity — so both private keys
must be accessible. Do NOT try to force `CODE_SIGN_IDENTITY="Apple Distribution"` on the archive: it
conflicts with automatic signing ("conflicting provisioning settings").
**The export step uses MANUAL signing**, not `-allowProvisioningUpdates` cloud signing: the ASC API
key cannot create a distribution provisioning profile ("Cloud signing permission error / No profiles
were found"), so the App Store profile is supplied as the base64 `APPLE_DIST_PROVISIONING_PROFILE`
secret, installed on the runner by the `Install App Store provisioning profile` step, and referenced
by name in `exportOptions.plist` (`signingStyle: manual`, `signingCertificate: Apple Distribution`,
`provisioningProfiles: { com.autom8ed.rgbsunglassesapp: <profile name> }`). The archive still uses
automatic signing (a Development profile, which the ASC key CAN cloud-create); only the export is
manual. When the distribution cert is rotated, regenerate this profile too.
This replaced an earlier
assumption of "cloud signing, no keychain setup" — that failed on the real runner: `codesign` hit
`errSecInternalComponent` because the runner's non-interactive session couldn't reach the login
keychain (and only the Distribution cert had been imported, so the Development-signed archive step
had no accessible key). Doing it in-job with both certs makes signing survive runner reboots /
headless sessions. A one-time `app-ios-ci.yml` per-push build still uses `CODE_SIGNING_ALLOWED=NO`,
so it never exercises signing — the release job is the only place signing runs.
Upload is a second `xcodebuild -exportArchive` with `destination: upload` in the exportOptions
plist (`altool` is deprecated). `ios.buildNumber` is injected at build time by a shared `version`
job (single source of truth — the same value is the Android versionCode): tag builds use
`MAJOR*10000 + MINOR*100 + PATCH` (≥ 10000; the job rejects 0.x.y tags), manual dispatch builds
must be 1–9999 (enforced, so the ranges can't collide). **TestFlight permanently consumes each
(version, buildNumber) pair** — never re-push an
app tag whose TestFlight upload succeeded; bump patch instead (see the release skill). Export
compliance is pre-answered by `ios.infoPlist.ITSAppUsesNonExemptEncryption: false` in `app.json`
(the app uses only standard OS encryption), so builds go straight to internal testers after Apple's
~5–30 min processing. `app-ios-ci.yml` (unsigned simulator build) is unchanged and remains the
per-push check. Optionally, `ios.appleTeamId` can be added to `app.json` for local
`expo run:ios --device` convenience — CI doesn't need it (the workflow passes `DEVELOPMENT_TEAM`
explicitly).

The in-app self-update flow is **disabled on iOS entirely** (it side-loads an APK, which only Android
can do) **and in Google Play builds** (Play policy forbids self-updating outside Play). The
`APP_SELF_UPDATE_SUPPORTED` flag in `services/app-update.ts` (Android + non-null Expo config +
`extra.distribution !== 'play'`; fails closed when the config is unavailable) gates every entry
point: no launch-time check or "update available" banner, the footer shows a plain version label
instead of a "Check for updates" link, and the update modal renders a "not available" message if
reached via deep link. On iOS the app is updated through the App Store / TestFlight; Play builds
are updated by Play itself. The Play variant is produced by the `play` job in
`.github/workflows/app-release.yml` via the shared `build-signed-android` composite action, which
stamps `extra.distribution = "play"` into `app.json` and strips `REQUEST_INSTALL_PACKAGES` — see
[docs/play-publishing.md](docs/play-publishing.md) for the full pipeline, signing-lineage rules
(the CI keystore is the Play app signing key — never rotate it casually), and the one-time Play
Console bootstrap.

**iOS has a dev-variant too** (`plugins/withDevVariantIos.js`, composed into `withDevVariant`
alongside the Android-only steps): a Debug-configuration build gets bundle id
`com.autom8ed.rgbsunglassesapp.dev`, home-screen label "RGB Glasses (Dev)", and the same dark
`appicon-dev.png` art Android uses (regenerated into a second `AppIcon-Dev.appiconset` from the
existing `AppIcon.appiconset`'s `Contents.json`, since `expo run:ios`'s Debug build and a
TestFlight/Release build can then coexist on one test device). Unlike Android, this is done
entirely via per-configuration Xcode build settings (`PRODUCT_BUNDLE_IDENTIFIER`,
`APP_DISPLAY_NAME` referenced from `app.json`'s `ios.infoPlist.CFBundleDisplayName` as
`$(APP_DISPLAY_NAME)`, `ASSETCATALOG_COMPILER_APPICON_NAME`) rather than a Gradle-style
per-buildType resource overlay — iOS's Debug/Release are configurations of one target sharing one
Info.plist, so there's no `src/debug` equivalent to overlay resources into. Only `Debug` gets the
`.dev`-suffixed id/name/icon; `Release` (the TestFlight archive, `-configuration Release` only) is
untouched.

Deliberately **not mirrored**: Android's `rgbsunglassesapp` → `rgbsunglassesapp.dev` URL-scheme
rewrite. That trick exists solely to avoid a chooser-dialog collision in `expo run:android`'s
deep-link launch path and to disambiguate the Android-only self-update deep link
(`services/app-update.ts`, gated by `APP_SELF_UPDATE_SUPPORTED` — no iOS equivalent exists).
`expo run:ios --device` doesn't launch via a scheme-based deep link at all — it reads the actual
`CFBundleIdentifier` out of the freshly-built `.app`'s Info.plist and installs/launches directly —
so there's no iOS consumer this would need to disambiguate for.

Because `.github/workflows/app-ios-ci.yml`'s simulator build uses `-configuration Debug`, its
smoke-test step's `BUNDLE_ID` is `com.autom8ed.rgbsunglassesapp.dev`, not the production id.

### Physical-iPhone dev builds + BLE (verified 2026-07-11, iPhone 15 / iOS 26.5, Xcode 26.2)

**Deploying a local dev build to a physical iPhone — use `app/scripts/launch-app-ios.sh --device <UDID>`,
never a bare `npx expo run:ios`.** The wrapper is the iOS sibling of `launch-app.sh`: it verifies the
`app` hardware lock (agent sessions), always re-runs `expo prebuild --platform ios` (incremental) so
config-plugin output lands no matter how old the checked-out `ios/` is, and then **asserts the Debug
configuration carries the `.dev` bundle id before building** — refusing loudly otherwise. That assert
exists because of a real incident (2026-08-13): a bare `expo run:ios` against a stale `ios/` predating
`withDevVariantIos` built Debug under the PRODUCTION bundle id and silently replaced the TestFlight
install on the shared iPhone. (Verified: the incremental prebuild fully retrofits even a tree that
predates the #320 rename — such a tree keeps its old `RGBSunglasses.*` project name, which is why the
wrapper globs `ios/*.xcodeproj` rather than hardcoding a name.) Run it as a harness-managed background
task, same as the Android wrapper.

- Pass the **traditional hardware UDID** from `xcrun xctrace list devices` (`00008120-…`), NOT the
  CoreDevice UUID that `xcrun devicectl list devices` prints — Expo CLI doesn't match the latter.
  (Expo also warns `Unexpected devicectl JSON version` on Xcode 26; device matching by UDID still works.)
- Expo refuses to build with "No code signing certificates are available" and will NOT mint one.
  First-time bootstrap: sign into Xcode → Settings → Accounts, make sure `ios.appleTeamId` is in
  `app.json` (prebuild bakes it into the project as `DEVELOPMENT_TEAM` — without it xcodebuild fails
  with "requires a development team"), then run
  `xcodebuild -workspace ios/RGBGlasses.xcworkspace -scheme RGBGlasses -configuration Debug
  -destination 'id=<UDID>' -allowProvisioningUpdates build` once — that creates the Apple Development
  cert + device-registered profile, after which `expo run:ios --device` works normally.
- Expo's auto-launch after install can silently no-op on physical devices; launch explicitly with
  `xcrun devicectl device process launch --device <UDID> com.autom8ed.rgbsunglassesapp.dev` —
  note the **`.dev`** suffix: the wrapper builds Debug, which installs under the dev-variant
  bundle id. Launching the bare production id here either errors ("no such app") or launches a
  leftover TestFlight install — and looks exactly like a failed deploy.
- **Local Network permission**: the app can't reach Metro (LAN IP in the app's `ip.txt`) until the
  user accepts iOS's local-network prompt on first launch — the symptom is "app launches, Metro never
  receives a bundle request". Phone and Mac must share the Wi-Fi network.
- execbro works against a physical iPhone at the **JS level only** (Metro CDP: `scan_metro`,
  `get_logs`, `execute_in_app`); screenshots/tap drivers are simulator-only.
- The board's serial shell from macOS: `/dev/cu.usbmodem*` at 115200 (interface 0 of 2fe3:0001 =
  lower-numbered node). The firmware dev loop (build → OTA flash → serial verify) runs natively on
  the Mac too — one-time setup via `scripts/macos-setup.sh`, details in `fw/CLAUDE.md`'s
  "macOS host" section. `scripts/hw-lock.sh` works on macOS (it re-execs into Homebrew bash ≥ 4),
  and the same lock discipline applies. Locks are per-host (`$GIT_COMMON_DIR`), which is fine:
  the board is physically attached to exactly one host at a time.

**iOS BLE behavior differences (all hardware-verified):**

- **First-time pairing leads discovery (re-verified 2026-08-13, iPhone iOS 26.6, post-#232 firmware)**:
  the firmware sends its SMP Security Request from the `connected()` callback, and iOS surfaces the
  passkey pairing dialog ~1 s after connect — BEFORE discovery runs. iOS holds ATT traffic while the
  pairing is pending (passkey entry took ~14 s in testing; zero authentication/encryption errors in
  the whole discovery pass), then discovery runs once, fully encrypted: every characteristic named
  and valued, monitors up, **no manual disconnect+reconnect needed**. This replaces the pre-#232
  behavior (full unauthenticated discovery pass with every read failing `attErrorCode: 5`, dialog
  only afterwards, manual reconnect required — issue #137, now closed). If the user ignores the
  dialog, SMP times out ~30 s in and the connect fails CLEANLY (firmware: `SMP Timeout` → disconnect
  reason 22; app: connect error → row returns to Connect; retry works). The passkey is printed only
  on the board's serial console (`Passkey for <addr>: NNNNNN`) — read it there and enter it on the
  phone within the ~30 s window.
- **ATT MTU on iOS is 293** (iPhone 15/iOS 26) — iOS negotiates on its own; `requestMTU(247)` is a
  no-op there. Comfortable headroom over Android's 247; zero `bt_att: No ATT channel for MTU`
  warnings in a multi-hour session with all 35 monitors streaming.
- **Discovery is slower on iOS** (~30-55 s vs ~6 s on Android) even with the bulk-metadata path
  working (it does work, verified) — iOS has no `requestConnectionPriority` equivalent and the
  firmware's `bt_conn_le_param_update()` still yields a 15 ms interval, so the gap is likely
  iOS-side GATT scheduling. Unoptimized as of 2026-07; measure before assuming regressions.
- **First-launch scan now waits for PoweredOn (issue #136, fixed)**: `startBluetoothScan` gates on
  `bleManager.state()` / `onStateChange` before scanning, because CoreBluetooth sits in `Unknown`
  while initializing — and on the app's very first launch the Bluetooth permission prompt holds it
  there for as long as the user takes to answer (hardware-verified: the ungated scan AND its old
  fixed 2 s retry both died with `BluetoothLE is in unknown state`, leaving a dead empty screen
  until refocus). `Unauthorized`/`Unsupported` drop to the empty state instead of waiting forever;
  `PoweredOff` waits, so flipping Bluetooth back on auto-starts the scan. The wait subscription is
  generation-guarded and removed in the focus-effect cleanup (`stateSubRef`).
- **`device.id` is NOT a MAC on iOS**: CoreBluetooth never exposes BLE MAC addresses; ble-plx's
  `device.id` there is an opaque per-phone peripheral UUID (can change if the bond is forgotten).
  Everything keyed off "macAddress" in the app still works (it's just an opaque key), but don't
  *display* it on iOS (`bluetooth-device-list-item.tsx` hides the caption there) and don't write
  iOS logic that expects `AA:BB:CC:DD:EE:FF` format.
- **Text-based parameter inputs commit on `onEndEditing` on iOS** — all three (uint32, float32,
  utf8) via the shared `characteristic-text-input-base.tsx`: iOS's number-pad/decimal-pad keyboards
  have **no return key**, so `onSubmitEditing` is unreachable there — dismissing the keyboard (tap
  outside the field) is the commit signal on iOS, uniformly across field types. Android is unchanged
  (✓/Return submits via `onSubmitEditing`, tap-away cancels). Two subtleties baked into the base,
  both regression-tested in `__tests__/characteristic-inputs.test.tsx`:
  - The no-op-edit skip compares **display strings** (`decodeToDisplay(charInfo.value) ===
    pendingValue`), never re-encoded bytes: float32 display is rounded to 7 significant digits
    (`formatFloat32`), so re-encoding it can differ from the stored bytes by 1 ULP — a byte compare
    turned a casual tap-in/tap-out into a BLE write that corrupted the stored value.
  - A `submittedRef` suppresses the blur-after-submit double-fire when a return key IS available
    (iOS text keyboard, hardware keyboards) — without it one commit sends two BLE writes.
  Decided against an `InputAccessoryView` "Done" bar (rejected in review: extra chrome) and against
  `numbers-and-punctuation` keyboard (full keyboard for a number field).
- **Core Bluetooth state restoration re-adoption (issue #190)**: when iOS jetsams the app while a
  board is connected, Core Bluetooth relaunches it in the background on the next BLE event for that
  peripheral. The restore callback (`restoreStateFunction` in [hooks/ble-manager.ts](hooks/ble-manager.ts))
  is registered at module-import time but **delivered asynchronously** (a native bridge event that can
  land before or after React mounts), so the handoff is a stash-or-deliver, deliver-once subscription
  (`subscribeRestoredPeripheral`), not a read-once peek; `useBleRestorationAdopt`
  ([hooks/use-ble-restoration.ts](hooks/use-ble-restoration.ts), mounted as `BleRestorationAdopter` in
  the root layout inside `BluetoothProvider`) receives it whichever ordering wins and drives
  the issue-#124 `startReconnectLoop` (once-only — a duplicate start would bump the reconnect
  generation and tear down the restored link) — the iOS pending connect resolves immediately on the
  still-connected peripheral, then the normal discovery/monitor/selection path runs (fast: iOS serves
  it from its native GATT cache on a live link). Two deliberate limits: **restoration never fires
  after a user force-quit** (App Switcher swipe) — iOS only relaunches after a *system* termination
  (platform limitation, the user must reopen the app) — and **ordinary cold launches do not
  auto-connect** (no last-device persistence; scope decision on #190, the restored-peripheral handoff
  is the only cross-launch state). Hardware-verified 2026-07-18 (iPhone 15/iOS 26.5): SIGKILL of the
  backgrounded app → relaunch within seconds on the board's next notify, restore→re-adopt→reconnected
  on attempt 1, board never saw a disconnect (stayed CONNECTED/L4/MTU 293), background Metro bundle
  fetch worked, app→device write round-trip confirmed via serial. One surprise to not misread during
  future debugging: ~50 s **after a user force-quit**, iOS's SYSTEM stack briefly reconnected to the
  bonded board (serial showed a bonded L4 connection with NO app process alive) and dropped it ~15 s
  later — the iOS analog of OxygenOS's system-level auto-connect (see the Android note above). A
  bonded L4 connection on serial does not by itself mean the app is running or was relaunched.

### Android Permissions

Android 12+ (API 31+) requires `BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT`, and `ACCESS_FINE_LOCATION`. Permission handling in [hooks/ble-manager.ts](hooks/ble-manager.ts) `requestPermissions()`.

### Debugging BLE

- Verbose BLE logging is already on in dev builds: `if (__DEV__) bleManager.setLogLevel(LogLevel.Verbose)` in [bluetooth.tsx](<app/(tabs)/bluetooth.tsx>)
- Device name filter: `device.localName?.includes("RGB Sunglasses")` — the FIRMWARE's advertised name (`CONFIG_BT_DEVICE_NAME`), deliberately NOT renamed alongside the app's "RGB Glasses" label; changing it without a matching firmware release makes every device undiscoverable
- Monitor subscription in `BluetoothDeviceListItem` for live characteristic updates

## Common Patterns

### Adding New Characteristic Types

Follow the `/add-gatt-characteristic` skill (`.claude/skills/add-gatt-characteristic/`, app steps in its `references/app-side.md`) — do not improvise. The files involved:

1. CPF format constant: [constants/bluetooth.ts](constants/bluetooth.ts)
2. Decode + dispatch: [hooks/use-characteristic-editor.tsx](hooks/use-characteristic-editor.tsx) — `decodeValueForInput()` (decode), `renderCharacteristicInput()` (per-format control dispatch), `pendingValues` state for user-editable inputs
3. Encode helpers: [services/ble-value-codec.ts](services/ble-value-codec.ts)
4. The per-format UI component: `components/characteristic-*.tsx`

### State Updates with Optimistic UI

Always follow this pattern (the real implementation is `writeToCharacteristic`/`writeServiceCharacteristic` in [context/bluetooth-context.tsx](context/bluetooth-context.tsx) — see "BLE Optimistic UI and Notification Behaviour" below for the exact ordering rules):

```typescript
const previousValue = charInfo.value ?? "";
const encoded = btoa(newValue);
setCharUpdateInProgress(charUuid, true);
charInfo.characteristic
  .writeWithResponse(encoded)
  .then(() => updateCharValue(charUuid, encoded))
  .catch(() => updateCharValue(charUuid, previousValue)) // Revert on error
  .finally(() => setCharUpdateInProgress(charUuid, false));
```

### Color Encoding

RGB colors are uint32, wire bytes little-endian `b,g,r,mode`. Byte 3 is the **color mode** (issue #259), mirroring the `ColorMode` enum in `fw/src/animations/color_mode_source.h`:

| mode | meaning | bytes 0-2 |
|---|---|---|
| 0x00 | Static | `b,g,r` — the color, as before |
| 0x01 | Spectrum Sweep | byte 2 (r) = speed 0-255, others 0 |
| 0x02 | Random on Beat | reserved (0) |
| 0x03 | Random on Activate | reserved (0) |
| 0x04 | Random Timer Fade | byte 2 (r) = speed 0-255, others 0 |

**Any unknown mode byte decodes as Static** — including 0xFF, the upper byte of the `0xFFFFFFFF` default persisted on pre-#259 devices. Mode-aware codec: `encodeColorValueToBase64`/`decodeColorValueFromBase64` (`ColorValue = {mode, rgb, speed}`); the legacy `encodeColorToBase64`/`decodeColorFromBase64` remain as byte-identical static-mode wrappers. All in [services/ble-value-codec.ts](services/ble-value-codec.ts); HSV↔RGB conversion in [color-picker-modal.tsx](app/color-picker-modal.tsx); mode constants + labels in [constants/bluetooth.ts](constants/bluetooth.ts).

## Known Issues & Quirks

- **Scan must stop before connecting**: `connect()` in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts) calls `bleManager.stopDeviceScan()` before `connectToDevice()` (previously documented here but not actually enforced in code — a scan running concurrently with `connectToDevice()` could get the connect operation itself cancelled by the OS/library even though the native link completed, leaving the app thinking it failed while the board thinks it's connected and has stopped advertising).
- **Orphaned BLE scans leak Android's scan-client registrations**: [app/(tabs)/bluetooth.tsx](<app/(tabs)/bluetooth.tsx>)'s `useFocusEffect` starts scanning via an unawaited async `startBluetoothScan()`, which itself awaits `requestPermissions()` (several native round-trips) before actually calling `bleManager.startDeviceScan()`. If the screen loses focus during that await, the effect's synchronous cleanup runs as a no-op (nothing is scanning yet) — then the pending promise resolves and starts a scan anyway, with no cleanup left to ever stop it. `react-native-ble-plx` does not stop a prior scan when a new one starts (both the JS layer and the Android native module just overwrite the internal subscription), so each orphaned scan permanently consumes one of Android's small number of concurrent scan-client slots, eventually producing `SCAN_FAILED_APPLICATION_REGISTRATION_FAILED` (error code 6). Fixed with a per-invocation **generation token** (`scanGenRef`): the focus effect bumps it on both focus and cleanup, each `startBluetoothScan(gen)` captures its value and bails after any `await` (and inside the scan callback) once the counter has moved on, so only the newest invocation ever touches the scanner — this correctly handles a fast blur→refocus that a shared boolean can't (a refocus would reset the boolean and let a superseded invocation through). Plus a defensive `stopDeviceScan()` at the top of `startBluetoothScan()`, and the failure-retry `setTimeout` handle is stored in a ref and `clearTimeout`d in the cleanup so a pending retry can't fire into the next focus session and start a second concurrent scan.
- **McuMgr responses are fragmented**: Read multiple notifications until `moreData` flag is false
- **Base64 encoding everywhere**: All BLE characteristic values are base64, even booleans
- **React Native Reanimated**: Required for navigation animations but causes Metro bundler warnings (safe to ignore)
- **Patch package**: `postinstall` script applies BLE PLX patch automatically
- **Stale Android GATT cache after firmware GATT restructuring**: Android persists a handle-based attribute cache per bonded device. Adding/removing a BLE service or characteristic in firmware shifts attribute handles for everything declared afterward in the GATT database, so a previously-bonded phone can read descriptors by the wrong (now-stale) handle, failing with `GATT_INVALID_HANDLE`. **`connect()` no longer uses `refreshGatt` to paper over this** — it was tried (`connectToDevice(..., { refreshGatt: 'OnConnected' })`, calling Android's `BluetoothGatt.refresh()` before discovery) but hardware testing proved it does NOT rescue the stale-cache case on a non-compliant OEM stack (the connection hangs regardless) while taxing every healthy connect, so it was dropped. See the two entries below — "Connect is sequenced link → MTU → discover, with NO `refreshGatt`" and the two-phone split-brain writeup — for the current approach and the real recovery (forget + re-pair on a non-compliant stack; stock Android recovers on its own via the firmware's Service Changed indication).
- **A failed per-item BLE read during discovery can orphan the connection**: in `connect()`'s discovery loop, descriptor/characteristic reads are wrapped in their own try/catch and skip-on-failure (rather than letting one bad read abort the whole function) — see [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts). The outer `catch` in `connect()` also explicitly calls `bleManager.cancelDeviceConnection()`. Without that, a thrown error during discovery leaves the native BLE link connected at the OS level (so the device stops advertising) while the app's state still thinks it's disconnected — the device then can't be found again by scanning, and the only way out is to force-stop the app (or kill its process) so the OS notices the client is gone and drops the link.
- **`patch-package` has two very different invocations — don't confuse them**: bare `npx patch-package` (no args) _applies_ every patch file under `patches/` to a clean `node_modules`. `npx patch-package <package-name>` _regenerates_ that package's patch file by diffing the current (possibly already-hand-edited) `node_modules` against a fresh install — i.e. it's a "save", not a "reapply". Running the regenerate form against an already-patched tree overwrites the patch file with a huge unintended diff. To extend an existing patch: reinstall a clean copy of the package, apply existing patches (`npx patch-package`, no args), make the new edit directly in `node_modules`, then regenerate (`npx patch-package <package-name>`) and review the diff line-by-line before trusting it.
- **BLE notifications silently fail without a larger MTU**: `connect()` in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts) calls `deviceConnection.requestMTU(247)` as its own awaited step after the link is established (not inline in `connectToDevice()` — see the "Connect is sequenced" entry below for why it's separated and non-fatal). Without the bump, the connection stays at the BLE default `ATT_MTU` (23 bytes, ~20 usable). Unlike writes/reads (which transparently fragment large values via prepare/execute-write and blob-read), a single `bt_gatt_notify()` call cannot be split across multiple ATT PDUs — the whole value must fit in one MTU-bounded packet. A notifiable characteristic whose value exceeds the negotiated MTU fails firmware-side only (a `printk` warning, e.g. `bt_att: No ATT channel for MTU ...`), with no error surfaced to the app — the app just never receives the notification and silently keeps showing the old value. See the matching firmware-side note in `fw/CLAUDE.md` (`bt_service_cpp.h notify()`) — even with this MTU bump, a notifiable characteristic whose _content_ can grow past ~244 bytes (e.g. `Glim Selection` if the GLIM file count grows a lot) can still exceed the negotiated MTU and needs either a bigger `requestMTU`, a smaller payload, or an app-level read-after-notify pattern.
- **Initial connection/discovery is slow without a connection-priority bump (issue #41)**: the discovery loop in `connect()` does ~170+ sequential GATT reads (one `descriptorsForCharacteristic`/`descriptor.read()`/`characteristic.read()` round-trip per characteristic — can't be parallelized, Android only allows one outstanding GATT operation per connection at a time). Each round-trip takes roughly one full connection interval, and neither side requests a fast one by default (~30-50ms). `connect()` now calls `deviceConnection.requestConnectionPriority(ConnectionPriority.High)` (from `react-native-ble-plx`) right after `connectToDevice()` resolves, dropping the interval to ~7.5-15ms — roughly a 3-4x cut in discovery time. Android-only effect (no-op on iOS); wrapped in try/catch since it's non-fatal if it fails. The firmware makes a matching request from its side (`bt_conn_le_param_update()` in `fw/src/bluetooth.cpp`, see `fw/CLAUDE.md`) as a belt-and-suspenders fallback in case the app-side request doesn't take effect.
- **Bulk per-service metadata read, cutting discovery further (issue #41 follow-up)**: per service, `connect()` first looks for a characteristic matching `UUID_METADATA_CHARACTERISTIC` (`constants/bluetooth.ts`) — a firmware-synthesized characteristic (see `fw/CLAUDE.md`'s `bt_service_cpp.h` entry) whose value is a packed blob containing every sibling characteristic's CUD name + CPF format. If found, it's read once and parsed via `parseMetadataBlob()` (`services/ble-value-codec.ts`), then zipped _positionally_ onto that service's characteristic list — replacing what would otherwise be 2 descriptor reads (CUD + CPF) per characteristic with 1 read for the whole service. Falls back automatically to the original per-descriptor path on any read failure, blob-version mismatch, or entry-count mismatch (logged, never silently mis-zipped) — this is also what happens for services that don't have the characteristic at all, e.g. the third-party McuMgr service, or any firmware build with `CONFIG_APP_BT_METADATA_CHARACTERISTIC=n` (disabled on `rgb_sunglasses_dk` for flash-size reasons). Extension animation services (`fw/src/extensions/extension_bt.cpp`, issue #90 follow-up) now synthesize this same characteristic at runtime too — this hook needed no changes to pick them up, since it never special-cased built-in vs. extension services in the first place. The positional zip relies on `characteristicsForService()` returning characteristics in firmware GATT declaration order — true by the ATT spec's ascending-handle-order guarantee for characteristic discovery (see the ordering-assumption comment block in `use-ble-connection.ts` and the matching one in `fw/src/bluetooth/bt_service_cpp.h` for the full chain, including the one verified-but-not-enforced link: react-native-ble-plx's Android module passes Android's native discovery order through unmodified). Hardware-verified: total discovery time across all 9 services dropped from ~13-30s to ~6s, with every service correctly using the bulk path and zero fallback/mismatch warnings.

- **A stale Android GATT cache on a bonded device causes the "split-brain" (board LED solid, app times out) — but ONLY on non-spec-compliant OEM stacks; stock Android recovers on its own (issue #90, hardware-proven on two phones)**: Android caches a bonded device's GATT database (handles/services) per bond. Any firmware reflash that changes the GATT layout (adds/removes/reorders a service or characteristic) shifts handles and makes that cache stale. The spec-defined recovery is the firmware's Service Changed indication / Database Hash (`CONFIG_BT_GATT_SERVICE_CHANGED` + `CONFIG_BT_GATT_CACHING`, both on) telling the phone to re-discover. **Whether the phone honors it is device-dependent — verified directly by adding a characteristic, reflashing WITHOUT re-pairing, and watching:**
  - **Pixel 9 Pro (stock Android 16): honors it.** Re-discovered automatically, found the new characteristic, `bt_state` showed a healthy `ATT MTU: 498` link — no re-pair needed. This is correct/spec-compliant behavior and is what end users on stock Android (and presumably most AOSP-derived stacks) will experience across a firmware OTA.
  - **OnePlus 9 Pro (OxygenOS / Android 14): does NOT honor it.** The bonded reconnect **hangs** — link up + encrypted (`bt_state`: `CONNECTED`, `L4`) but stuck at `ATT MTU: 23`, and `requestMTU`/`discoverAllServicesAndCharacteristics` both time out. **No app-side connect option rescues it** — tested `refreshGatt` on/off and `requestMTU` before/after discovery; every combination hangs. The only fix is **Settings → Bluetooth → forget the device, then re-pair** (needs the user for the passkey — see the BLE-Pairing note above), which wipes the stale cache.
  - Practical rule: if the board LED is solid but the app can't discover *after a firmware reflash* and the scanner isn't the problem, it's a stale cache on a non-compliant stack — forget+re-pair. Fastest confirmation: the firmware `bt_state` shell command (see `fw/CLAUDE.md`) — `ATT MTU: 23` on a `CONNECTED`/`L4` link **is** the split-brain. The shared OnePlus dev phone hits this on every GATT-changing reflash; a Pixel does not.
- **Auto-reconnect + Android foreground service (issue #124)**: an UNEXPECTED disconnect (OS kill, radio loss, board reset — anything that fires `onDeviceDisconnected`) no longer reverts the UI to "Connect"; it starts an auto-reconnect supervision loop (`startReconnectLoop()` in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts)). Key semantics, all unit-tested:
  - The loop uses a **timeout-less pending connect** — `connectToDevice(mac, {autoConnect: true})` on Android, `connectToDevice(mac, {})` on iOS — which adopts the board the moment it's seen advertising again, with **no scanning** (scanning is suppressed while a reconnect is pending, and the reconnecting row is pinned in the Connect list). Backoff 2/5/10/30 s applies only to attempts that *error*; retries continue **indefinitely** while the app is alive. Every 3rd attempt hedges with a direct `{timeout: 60000}` connect in case OEM `autoConnect` is flaky.
  - **User-initiated disconnects never auto-reconnect** — structurally (`disconnect()` removes the listener before cancelling) plus a context-level `intentionalDisconnectRef` any future on-purpose drop (e.g. OTA reboot) can set.
  - The **"Reconnecting…" button is the cancel affordance** (tap = `cancelReconnect()`), since the loop never gives up on its own. Cancel bumps `reconnectGeneration` (context ref) — in-flight pending connects self-abort via generation snapshots even if they resolve after cancellation.
  - The per-row connect dedup moved to context (`connectPromises`, keyed by mac) so a user tap mid-reconnect **shares** the loop's in-flight attempt (the overlapping-`connectToDevice` split-brain guard now spans row remounts).
  - **Android FGS**: while connected, a `connectedDevice`-typed foreground service ([services/ble-foreground-service.ts](services/ble-foreground-service.ts), notifee) keeps the process + GATT link alive in the background. It is only ever **started** from the user-initiated connect path (Android 12+ bans background FGS starts); reconnects only update the notification text. Stopped on user disconnect/cancel. notifee's bundled service is declared `shortService` (≈3 min cap on Android 14) — [plugins/withBleForegroundService.js](plugins/withBleForegroundService.js) retypes it via `tools:replace`; if the notification ever vanishes ~3 min into backgrounding, suspect that plugin regressed. Play requires an FGS declaration — see docs/play-publishing.md.
  - **Foreground verify** ([hooks/use-ble-app-state.ts](hooks/use-ble-app-state.ts), mounted in the root layout): on AppState → active, if `selectedDevice` is set but `isDeviceConnected()` is false (disconnect event missed while suspended — the iOS case), it runs the disconnect-handler cleanup and starts the reconnect loop.
  - **Hardware-verified (2026-07-17, OnePlus 9 Pro)**: 12+ min backgrounded with the link CONNECTED/L4 and instant control on re-foreground; loop backoff/hedge sequencing; cancel stops the loop, aborts the pending connect, and stops the FGS. Three OxygenOS findings from that session:
    - **OxygenOS auto-connects bonded boards at the SYSTEM level** — an advertising bonded board gets grabbed by the phone's stack itself (bt_state: CONNECTED/L4 but `ATT MTU: 23`, no app GATT client). Looks exactly like the split-brain; it isn't the app's doing (survives force-stop). A normal Connect tap re-adopts it via ble-plx's cancel-then-connect.
    - **Bonded reconnects after a board reboot or a phone BT-stack restart WEDGE on this phone** (MTU exchange never completes → discovery times out). The auto-reconnect loop correctly keeps retrying against it, but only forget + re-pair actually recovers — same pathology class as the issue-#90 stale-cache split-brain, now known to trigger WITHOUT any GATT layout change. Spec-compliant stacks (Pixel) recover on their own.
    - **When `/re-pair`'s automated forget fails** ("bond still present") because a zombie system link or a pending app connect blocks unpair, the working recipe is: J-Link-halt the board (`halt` via JLinkExe — board lock required), `svc bluetooth disable` → `enable`, then IMMEDIATELY gear → Unpair (works only while the board is unreachable), then J-Link reset the board and `/re-pair --no-forget`.
- **Settings' "Unpair" silently no-ops while any client holds a pending LE connection to the device (observed 2026-07-17, OxygenOS / OnePlus 9 Pro)**: with the companion app running, its BleManager keeps a background connect to the bonded board permanently armed — `dumpsys bluetooth_manager` shows the board under `devices attempting connection`. In that state, tapping Unpair on the (correct, verified) device-details page does nothing: the UI navigates back as if it worked, but the bond survives, indefinitely. The same pending connect can also keep the board out of the app's own scan results even though the board is advertising (the stack is consuming its adv reports for the connect attempt). **Forget procedure that actually works: force-stop the app first (`adb shell am force-stop …`), and if a prior Unpair attempt already got the stack stuck, toggle Bluetooth off/on (`svc bluetooth disable`/`enable`) to drop the pending connect — then Unpair.** `scripts/re-pair.sh` does all of this automatically now (force-stop before forget; BT-cycle + one retry if the unpair doesn't stick). Repeatedly tapping Unpair without killing the client will never take.
- **Connect is sequenced link → MTU → discover, with NO `refreshGatt` (issue #90)**: `connect()` in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts) calls `connectToDevice(mac, { timeout: 15000 })` (no `refreshGatt`, no inline `requestMTU`), then `requestMTU(247)` as its own awaited step, then discovers. Rationale: (1) MTU as a separate non-fatal step means a slow/failed exchange can't blow the connect timeout, and reads/writes still work at any MTU (only large *notify* payloads need the 247 bump). (2) `refreshGatt:'OnConnected'` calls Android's `BluetoothGatt.refresh()`, which wipes the on-device cache and forces a full re-discovery on **every** connect — pure overhead when the cache is valid (the normal case). It was originally there to survive a firmware GATT change on a bonded phone, but the stale-cache testing above proved it does **not** help that case (it hangs too), so it bought nothing for the failure mode while taxing every healthy connect — dropped. `connect()` also retries `connectToDevice` once (force-closing the failed attempt's half-open GATT client first) for the controller-level first-attempt failure (HCI 0x3E / reason=62) on a just-rebooted bonded board.

## Testing Device Without Hardware

Connect to any BLE device with custom services to test UI rendering logic. The app gracefully handles missing descriptors by falling back to UUIDs.

## Autonomous Agent Notes (Claude / MCP)

### Two shared test phones — identify which is attached before trusting phone-specific notes

The bench phone is not always the same device: a **OnePlus 9 Pro (LE2125, OxygenOS /
Android 14)** and a **Pixel 9 Pro (stock Android 16)** rotate. Identify the attached one
first — `adb devices -l` (model field) or the `device` field in any execbro tool result —
and read the phone-specific sections below through that lens. Verified differences:

| | OnePlus 9 Pro (LE2125) | Pixel 9 Pro |
|---|---|---|
| BLE stack spec compliance | Non-compliant: ignores Service Changed; bonded reconnects **wedge** after a board reboot or GATT-changing reflash (`ATT MTU: 23` split-brain). Only recovery: forget + `/re-pair` (see the issue-#90/#124 entries above) | Compliant: honors Service Changed, re-discovers on its own after a GATT-changing reflash — no forget/re-pair needed (verified by adding a characteristic and reflashing) |
| Coordinate taps from the screenshot image | **Unreliable** — land high/short; use `tap(text=…, strategy="accessibility")` or the fiber-walk recipes | **Reliable** (verified 2026-07-31: repeated coordinate taps all landed); accessibility strategy and fiber-walk also work |
| Notification small-icon enforcement | Tolerant of a missing/invalid `smallIcon` | **Strict — a fatal app crash**, not a cosmetic issue: posting the BLE FGS notification without a resolvable `smallIcon` kills the process with `IllegalArgumentException: no valid small icon` (observed 2026-07-31 on every disconnect, because a stale `android/` lacked PR #224's `ic_stat_connection` drawable — `launch-app.sh` now always re-runs prebuild to prevent exactly this) |
| System-level bonded auto-connect | OxygenOS grabs an advertising bonded board at the system level (CONNECTED/L4, `ATT MTU: 23`, no app client) | Same phenomenon observed 2026-07-31 after a board power-cycle with a reconnect pending: serial showed CONNECTED/L4/MTU 498 while the app stayed "Reconnecting…" and the board never reappeared in scans (link held ⇒ board not advertising). Recovery: cancel the reconnect + `am force-stop` + relaunch + fresh Connect (or reset/power-cycle the board to free the link) |

Both phones: the fiber-walk `execute_in_app` recipes and `tap(text=…,
strategy="accessibility")` work — but accessibility/OCR matching only finds **on-screen**
elements, so scroll first for below-the-fold targets (e.g. the Battery card at the bottom
of Controls).

### Device-Free Validation Loop

For any app change that doesn't need the physical phone, run the `/validate-app` skill (`.claude/skills/validate-app/SKILL.md`): `npm ci` in `app/` (reapplies the ble-plx patch via `postinstall`), then jest + typecheck + lint. There is **no `typecheck` npm script** — it's `npx tsc --noEmit` directly. CI (`.github/workflows/app-ci.yml`) now gates all three — the `test` job runs jest, and a separate `typecheck-lint` job runs `tsc --noEmit` and `eslint --max-warnings 0` (added for issue #130, since a green CI used to mean only jest passed and tsc/lint debt drifted in undetected). Still run them locally before pushing so you're not waiting on CI to catch a type error or a new lint warning (any warning now fails CI). **Green here is not "verified"** — see the next section for the class of bug this loop is structurally blind to.

### What the device-free loop CANNOT catch: feedback between a write and the state it produces

Jest suites here mock `useBluetooth`, so `updateCharValue`/`updateServiceCharacteristicValue` are `jest.fn()`s that **never produce a new context object**. That breaks the loop between "code writes/reads a characteristic" and "context re-renders with fresh objects" — so any bug living in that loop is structurally invisible to green tests, no matter how many you add. Two such bugs shipped into a PR and were caught only on hardware (2026-08-05, PR #285):

1. **Unbounded BLE read loop.** A `useFocusEffect(useCallback(fn, [charInfo, updateCharValue]))` re-read a characteristic, called `updateCharValue`, got back a new `charInfo` identity, invalidated the callback, and re-ran the effect — measured at **110 reads in 10 s**, continuously, saturating the GATT queue. **Rule: any effect that both reads a characteristic and writes the result into context must take its inputs from `useRef`, and may depend only on values its own writes cannot change** — a plain string like `selectedDevice?.mac` is fine (and is how you re-arm on reconnect); anything context-derived (a `charInfo`, the device object, a context callback) closes the loop. `[]` is the common case, not the rule itself. Precedent: `components/battery-card.tsx` and `app/(tabs)/device-state/battery.tsx`.
2. **Deferred callback outliving its context.** A `setTimeout` read-back fired 150 ms after a write, by which time the characteristic could be torn down — `read()` then throws **synchronously**, which a bare `.catch()` does not handle, so a `TypeError` escaped as an unhandled error attributed to an unrelated test. **Rule: in any fire-and-forget deferred BLE callback, optional-call (`read?.()`) AND wrap in `try/catch`, not just `.catch()`.** Precedent: `scheduleClampReadBack` in `context/bluetooth-context.tsx`.

Two practical consequences:

- **A test asserting "the read happened" proves nothing about how many times it happens.** When adding a read-on-focus/interval path, also assert it does NOT re-fire: re-render with a *fresh* device object (mock `useBluetooth` with `mockImplementation`, not `mockReturnValue`, so each render returns new identities) and assert the read count is unchanged. Both suites above carry that test.
- **A deferred read must compare-and-swap before it applies.** By the time a delayed read-back resolves, a newer write or notification may already have set a fresher value; applying unconditionally snaps the control backwards until that newer write's own read-back lands. Patch through the function form and return `null` when the current value is no longer the one you wrote — the same guard the write-error revert path uses. Precedent: `scheduleClampReadBack` and the Active Animation fan-out in `hooks/use-ble-connection.ts` (which likewise skips services whose toggle did not change, rather than rewriting every service's state on each switch).
- **Anything in this class needs a real device before merge.** `adb logcat | grep -c "Read from Characteristic"` over a fixed window is the cheap check — steady-state BLE traffic should be near zero when the UI is idle. `/submit-pr` step 5 already mandates on-device verification for device↔app changes; this is one of the things to actually look for while there.

### App-Update Modal Auto-Opens After Force-Stop + Relaunch

After `adb shell am force-stop` + relaunch, the in-app self-update check runs on mount and can immediately push the **App Update** modal (`app-update-modal`) on top of the bluetooth tab if a newer release is found on GitHub. This leaves the navigation stack as `__root > (tabs) > app-update-modal > (tabs) > bluetooth`, which blocks tapping anything underneath. Dismiss it with `adb shell input keyevent KEYCODE_BACK` before trying to interact with the Bluetooth or Controls screens. The BLE connection (if triggered before the modal appeared) is still live — the button will show "Disconnect" once the modal is cleared.

### BLE Pairing — prefer the `/re-pair` skill

**Preferred: `scripts/re-pair.sh` / the `/re-pair` skill** automates forget + re-pair
hands-off. It arms a serial watcher on the board's shell UART, taps CONNECT, and a local
autoresponder types the board-displayed passkey into Android's dialog fast enough to beat
the reason-19 timeout — no human in the loop. Requires the `board` + `app` locks and any
MCP serial connection closed first (see the skill). Use it for the OnePlus stale-GATT
split-brain (`/debug-ble`) and any re-pair.

**Manual fallback** (if `/re-pair` can't drive the phone, e.g. Settings-UI drift on the
forget step). First-time pairing accepts Android system prompts that are timing-sensitive:

1. After tapping CONNECT in the app, Android shows a **"Pairing request"** notification in the status bar shade.
2. Swipe down → tap the pairing notification, then enter the 6-digit code the board prints on serial (`Passkey for … : NNNNNN`) into the PIN dialog. Since the issue #232 firmware fix (`CONFIG_BT_SMP_SC_ONLY` + early L4 request) there is a **single, PIN-code prompt** — the old consent-only "Pair & connect" step no longer precedes it. If the firmware log shows `Pairing failed` without a disconnect, that's the firmware rejecting a raced-in unauthenticated attempt; Android retries with the PIN dialog on the same connection — keep going.
3. All of this must happen before Android times out waiting for user input and drops the connection (`BT_HCI_ERR_REMOTE_USER_TERM_CONN`, disconnect reason 19). A failed/cancelled PIN entry now also gets a firmware-side disconnect (`BT_HCI_ERR_AUTH_FAIL`) instead of lingering at L1.

If a device has never been paired and `/re-pair` isn't being used, ask the user to watch for and accept the Android pairing prompts themselves. Once paired, subsequent connections complete automatically without any prompts.

### ADB Wireless Pairing State Lives on the Phone, Not the Container

`adb devices` showing empty does **not** mean the device was never paired. Wireless debugging pairing (the 6-digit code flow) is remembered by the phone; only the TCP connection itself is container-local and drops on container restart. Don't infer "needs full re-pair" from missing local files like `~/.android/known_devices.xml` — those don't reliably reflect pairing state either. Always try `adb connect <ip:port>` first (ask the user for the device's current IP:port from the Wireless debugging screen if unknown); only walk through the full `adb pair` flow if `adb connect` actually fails.

### Launching the App

**Hold the `app` hardware lock first — and use `app/scripts/launch-app.sh`, not raw `npx expo run:android`.** There is only one physical phone shared across every agent worktree. `hold` is the only way to take the lock (see root `CLAUDE.md` "Hardware locking" / `.claude/skills/hw-lock/SKILL.md`):

```
Monitor(command: "scripts/hw-lock.sh hold app", persistent: true)
```
```bash
timeout 15 bash -c 'until scripts/hw-lock.sh check app >/dev/null 2>&1; do sleep 0.5; done'
```

`app/scripts/launch-app.sh` no longer acquires this lock itself — it only verifies it's already held and hard-refuses to run otherwise, and refuses to start a second Metro instance even from this same session if you forgot one is already running. The relationship to the lock's own lifetime is asymmetric, not fully independent: stopping the `hold` task (or a same-session `release app --force`) now also stops Metro if it's still running — the script records its own pid against the lock right before exec-ing into Metro, so release can find and stop it precisely — meaning releasing the lock reliably guarantees Metro has quit. It doesn't run in reverse, though: Metro stopping or crashing on its own still does NOT release the lock — you still manage that side yourself. If a Metro/expo process is still running from an earlier, now-dead session when `app` is next held, that hold detects and kills it automatically before considering itself established. A `PreToolUse` hook also auto-denies `mcp__execbro__*` calls and `adb`/`expo run:android` in Bash without the lock. **Never call `npx expo run:android` directly** — doing so bypasses the lock and the single-Metro-instance guarantee, reintroducing exactly the collision risk this exists to prevent.

`app/scripts/launch-app.sh` (which runs `npx expo run:android` internally) is a blocking command — always run it as a background task. Use `--device <device name>` (the model name, not the ADB IP:port format):

```bash
app/scripts/launch-app.sh --device <device name>
```

**Never pass an ADB `ip:port` to `--device`.** Expo CLI matches `--device` against its _own_ device list (model/AVD names), not ADB serials, so a wireless target like `--device 192.168.1.34:41181` fails immediately with `CommandError: Could not find device with name: <ip:port>`. Pass the model name (`Pixel_9_Pro`, `LE2125`, …). With exactly one device attached (check `adb devices`), you can also **omit `--device` entirely** and Expo auto-selects it — handy for a wirelessly-connected phone whose model name you don't have to hand.

Poll `http://localhost:8081/status` until Metro reports `packager-status:running` before trying to interact with the app.

#### Running the app from inside a git worktree — MANDATORY procedure (read before doing anything)

A fresh worktree (`.claude/worktrees/<name>/app`) has **no `node_modules` and no `android/` of its own.** There is exactly one correct way to run the app from it. Follow it verbatim; the tempting shortcuts below are all forbidden because each one has already broken a session.

**DO — the only supported sequence:**

1. `cd <worktree>/app && npm ci` — a **real** install into the worktree. Takes ~30s and reapplies the ble-plx patch via `postinstall`. This is required for `jest`/`tsc` **and** for Metro. Eat this cost.
2. Hold the `app` lock first: `Monitor(command: "scripts/hw-lock.sh hold app", persistent: true)`, then confirm with a short `check app` poll (see "Launching the App" above).
3. `app/scripts/launch-app.sh` — launched as a **harness-managed background task** (Bash `run_in_background: true`). It verifies the `app` lock is held (refuses otherwise), then runs `expo prebuild` (generates `android/`), builds via gradle (fast once the shared gradle cache is warm — ~1 min), starts Metro, installs the APK, and launches the app pointing at its own Metro. Leave it running for the whole session — it owns Metro, but does **not** own the lock; stop the `hold` task separately when you're done with the phone.
4. Poll `http://localhost:8081/status` for `packager-status:running`, then screenshot to confirm the app loaded.

**DON'T — every one of these has caused a failure, do not attempt any of them:**

- **NEVER call `npx expo run:android` directly** — always go through `app/scripts/launch-app.sh`. Calling npx directly bypasses the `app` hardware lock check and the single-Metro-instance guarantee, so a second agent (or a forgotten earlier launch in this same session) can start a colliding second Metro instance against the one physical phone.
- **NEVER symlink `node_modules`** from the main checkout into the worktree (`ln -s <main-checkout>/app/node_modules ...`). The gradle build tolerates it, but **Metro's JS resolver cannot resolve modules through a symlink whose realpath is outside the worktree project root** — you get `UnableToResolveError: Unable to resolve module ./app/node_modules/expo-router/entry` and a red-screen `development server returned response error code: 404` on the device. Always `npm ci` for a real `node_modules`.
- **NEVER background `launch-app.sh` by hand with `&` and/or `> log 2>&1`.** That daemonizes it yourself, the harness sees the wrapper "complete" immediately, loses track of the task, and Metro gets reaped — the app then can't fetch its bundle. Use Bash `run_in_background: true` with the bare command (no `&`, no redirect) so the harness keeps it alive as a tracked task.
- **NEVER substitute `expo start --dev-client` + `adb reverse` + a `rgbsunglassesapp.dev://expo-development-client/?url=...` deep link** to avoid the native build. The installed dev client resumes its stale bundle without re-fetching, the deep link doesn't reliably trigger a fresh bundle against the new Metro, and you burn more time than a build would cost. Also don't pass `--android` to a separate `expo start` (it launches Expo Go, not the dev client).
- **NEVER kill the underlying `expo run:android` process directly** to "restart Metro." If Metro seems wrong, fix the actual cause (usually a stale/symlinked `node_modules`), then stop the `launch-app.sh` background task and relaunch it — this no longer touches the `app` lock either way, since launch-app.sh doesn't own it.

In short: in a worktree, **`npm ci`, then hold `app` via `Monitor`, then `app/scripts/launch-app.sh` as a background task.** No symlinks, no manual daemonizing, no `expo start` deep-link dance. Eat the full build cost — it is cheaper than every workaround.

#### After a FRESH install, the app sits at Android's runtime-permission dialog — grant it before any BLE automation

A first launch on a newly-installed APK (fresh device, or after `pm uninstall`/"kill the app +
re-deploy") blocks on Android's **nearby-devices permission dialog** ("Allow RGB Glasses (Dev)
to find, connect to, and determine the relative position of nearby devices?"). Until it's granted
the app can scan nothing, so every BLE automation downstream fails in a way that does **not**
mention permissions and looks like a hardware/firmware problem instead — observed 2026-07-26:
`scripts/re-pair.sh` reported `WARN: board 'RGB Sunglasses Proto0 94E0' not listed / Connect not
tappable within 25s` and gave up, purely because the dialog was still up. A screenshot is the fast
way to tell (`mcp__execbro__android_screenshot`); the dialog belongs to
`com.android.permissioncontroller`, not the app, so app-level component queries won't surface it.

Grant it either way:

```bash
# Pre-grant before launching, so no dialog ever appears (preferred for unattended runs)
adb shell pm grant com.autom8ed.rgbsunglassesapp.dev android.permission.BLUETOOTH_SCAN
adb shell pm grant com.autom8ed.rgbsunglassesapp.dev android.permission.BLUETOOTH_CONNECT
adb shell pm grant com.autom8ed.rgbsunglassesapp.dev android.permission.ACCESS_FINE_LOCATION
```

or tap it: `tap(testID="com.android.permissioncontroller:id/permission_allow_button")`. Note
`pm uninstall` also **revokes** previously-granted permissions, so a reinstall always re-arms this
— budget for it whenever you redeploy, and don't interpret the resulting "device not listed" as a
BLE fault before ruling it out.

**Root cause of `CommandError: No development build (com.autom8ed.rgbsunglassesapp) for this project is installed`, and the real fix (not a workaround)**: this project's `plugins/withDevVariant.js` config plugin intentionally injects `applicationIdSuffix ".dev"` into the debug build type (`android/app/build.gradle`) so the debug and release APKs can be installed side-by-side with distinct icons/schemes — the actual installed runtime package id is `com.autom8ed.rgbsunglassesapp.dev`, not the bare `applicationId`. Expo CLI's package-id resolver (`@expo/config-plugins`'s `Package.getApplicationIdAsync()`, called from `AndroidAppIdResolver`) only regexes the literal `applicationId '...'` line out of `build.gradle` — it has no knowledge of per-buildType `applicationIdSuffix`. So `expo run:android` always computes the unsuffixed id, checks whether _that_ is installed (`PlatformManager.openProjectInCustomRuntimeAsync` → `isAppInstalledAndIfSoReturnContainerPathForIOSAsync`), finds it isn't (only the suffixed `.dev` one is), and throws — even immediately after its own build+install step succeeded. This will happen on every `expo run:android` invocation as long as `withDevVariant.js`'s suffix exists, build success or not.

Expo CLI has a built-in flag for exactly this situation — `--app-id <appId>` — which makes it check/install/launch the given id instead of guessing one from `build.gradle`:

```bash
app/scripts/launch-app.sh --device <device name>
```

This is the correct fix to reach for, not the manual `adb install` + `monkey`/`android_launch_app` dance — that manual path still works as a fallback (e.g. if Metro itself won't start), but `--app-id` fixes the actual CLI invocation so it works end-to-end unattended. Don't pass `--android` to a separately-running `npx expo start` to reconnect Metro — that flag tries to auto-launch generic Expo Go instead of the custom dev-client app that's actually installed.

### BLE Link Can Get Orphaned by App Reloads, Not Just Discovery Failures

The "failed per-item BLE read during discovery can orphan the connection" entry in Known Issues & Quirks above covers one trigger. A second, distinct trigger hit repeatedly in this session: reloading the app mid-session (`mcp__execbro__reload_app`, or a firmware-side J-Link reflash/reset while the phone was connected) can leave the **native BLE link** connected at the OS level even though the app's own JS state has been wiped — the device then stops advertising and can't be found by a fresh scan, no matter how long you wait. The fix is the same as the discovery-failure case: `adb shell am force-stop <package>` (then relaunch) so the OS notices the client process is gone and drops the link. Don't waste time waiting longer for the device to reappear in a scan — if `Setting up characteristic monitors...`/a fresh `connect()` cycle hasn't run and the Bluetooth tab is stuck on the "RGB Glasses / Connect over Bluetooth" hero with no device listed for more than a few seconds, force-stop immediately. Note that Fast Refresh (HMR) is not one of these triggers: it preserves JS module state (including the module-scope `bleManager` singleton in [hooks/ble-manager.ts](hooks/ble-manager.ts)) and has not been observed to orphan the link (as of 2026-07); the confirmed triggers are a **full** JS reload (`mcp__execbro__reload_app` / dev-menu Reload) or a firmware reflash/reset while connected.

### A third split-brain trigger: overlapping `connectToDevice()` calls for the same device (issue #90 follow-up)

A third distinct trigger for the same native-connected/JS-disconnected split-brain: calling `bleManager.connectToDevice()` a **second** time for the same MAC address while a first call is still pending. `react-native-ble-plx`'s Android native module tracks one pending subscription per device (`DisposableMap`, `connectingDevices`, in `BleModule.java`) and its `replaceSubscription()` **unconditionally disposes whatever was already stored under that key** before storing the new one — so the *first* `connectToDevice()` promise rejects with `BleErrorCode.OperationCancelled` ("Operation was cancelled", errorCode 2) via a `doFinally` cleanup path, while the *second* call's `establishConnection()` is what actually completes on the real `BluetoothGatt`. The firmware sees a normal, successful connection (confirm via `bt_conn_info` — fast connection interval); the app just sees the first call's promise reject.

`connect()` in [hooks/use-ble-connection.ts](hooks/use-ble-connection.ts) now guards against this with a `connectingRef` (a `useRef`, checked and set **synchronously** at the very top of `connect()`, before the first `await`) — `isConnecting` **state** alone isn't sufficient, because state updates are asynchronous: a second `onPress` delivered before the component re-renders sees `isConnecting` still `false` and the button's `disabled={isConnecting}` hasn't taken effect yet, so both calls can reach `bleManager.connectToDevice()` for the same macAddress. Hit exactly this way during testing: a `tap()` on the "Connect" button (via `mcp__execbro__tap`) can deliver more than one `onPress` in the same tick as a fast double-tap would.

If you see `Operation was cancelled` on `connectToDevice()` with no scan-related error in the logs (ruling out the scan-leak class of bug above) and `bt_conn_info` on the firmware shows a genuinely live connection, suspect this mechanism specifically, not a fresh new bug — check whether `connect()` was somehow invoked twice for the same device before concluding otherwise.

### MCP Coordinate Systems

**Driving the app on a phone at all — which tap strategy to use, and how to wait for a
screen change — is `/drive-app`. Read it before a validation run; it is the difference
between a five-minute click-through and a half-hour of taps that silently press buttons
on the screen underneath.** This section is only the coordinate arithmetic.

Two coordinate spaces exist and are NOT interchangeable:

| Tool / context                         | Space             | Dimensions (Pixel 9 Pro)                    |
| -------------------------------------- | ----------------- | ------------------------------------------- |
| `android_screenshot()` delivered image | **execbro px**    | 896 × 2000                                  |
| `tap(x, y)`                            | **execbro px**    | same                                        |
| `get_screen_state` / `get_screen_layout` / `measure` | **execbro px** | same                     |
| `inspect_at_point(x, y)`               | **execbro px**    | same                                        |
| ADB `input tap` / `native=true` / `uiautomator dump` bounds | **raw device px** | 960 × 2142                |

```
device_px = execbro_px × 960/896   (exactly 15/14, ×1.0714)
```

`inspect_at_point` takes **execbro px, not dp** — corrected 2026-08-07 against the live
phone (`inspect_at_point(447, 1040)` returned the button under the finger; the
dp-converted `(213, 495)` returned an unrelated `Card`). An earlier version of this table
claimed dp and a `× 0.476` conversion; both were wrong. Verified three ways: `wm size`,
a raw `screencap` PNG header, and execbro's own `convertedTo` echo.

**Status bar**: 153 execbro px at the top; app content starts below it. `measureInWindow`
dp coordinates are relative to the content area (y=0 is below the status bar).

**Practical rule**: take coordinates verbatim from `get_screen_state`, and never scale
them. The third space — the image as rendered in your context, ~703 × 1568 — is never a
valid tap input; estimating off it misses, and a miss can land on a covered screen.

### execbro tapping on the OnePlus 9 Pro (LE2125) — use `strategy="accessibility"` first

(OnePlus-specific — on the Pixel 9 Pro coordinate taps work fine; see the two-phones
table above.)

**The most reliable approach on this device is `tap(text="...", strategy="accessibility")`.** It fires directly via the Android accessibility tree without any coordinate conversion ambiguity, and it worked in every verified session. Try this first for any button/link with a visible label.

Two things that do **not** work reliably:

- The **pressables list** that `android_screenshot` prints (e.g. `<AppButton/> "Connect" frame:(714,709 ...)`) reports coordinates that are _inflated_ relative to the delivered image — passing them to `tap(x,y)` lands high/short and misses.
- `tap(..., native=true)` and coordinate taps from the screenshot image also misfired repeatedly across multiple sessions. The crosshair appears where the tap landed but the resulting position in the delivered image is inconsistent.

**When `strategy="accessibility"` can't distinguish between two elements with the same label** (e.g. a "Connect" `AppButton` and a "Connect" `BottomTabItem` both matching), use `execute_in_app` to walk the fiber tree and fire `onPress` directly:

```javascript
// Find the first AppButton whose title matches, fire its onPress
(function() {
  var hook = globalThis.__REACT_DEVTOOLS_GLOBAL_HOOK__;
  var roots = hook.getFiberRoots(1);
  var root = null;
  roots.forEach(function(r) { if (!root) root = r; });
  var q = [root.current];
  while (q.length) {
    var f = q.shift();
    if (!f) continue;
    var n = f.type && (f.type.displayName || f.type.name || '');
    if (n === 'AppButton') {
      var p = f.memoizedProps || {};
      if (p.title === 'Connect' && p.onPress) { p.onPress(); return 'fired'; }
    }
    if (f.child) q.push(f.child);
    if (f.sibling) q.push(f.sibling);
  }
  return 'not found';
})()
```

Note: `BaseExpoRouterLink` children are not a bare string in the fiber props — `tap(text=..., strategy="accessibility")` handles those correctly without needing fiber tree surgery.

### Toggling Switch (Boolean) Characteristics

`Switch` components use `onValueChange`, not `onPress`, so they cannot be triggered via `tap()` by component name or coordinates. Instead, walk the React fiber tree and call the component's `onWrite` prop directly:

```javascript
// In execute_in_app:
(function () {
  var hook = globalThis.__REACT_DEVTOOLS_GLOBAL_HOOK__;
  var fiberRoots = hook.getFiberRoots(1);
  var firstRoot = null;
  fiberRoots.forEach(function (r) {
    if (!firstRoot) firstRoot = r;
  });

  var target = null;
  var queue = [firstRoot.current];
  while (queue.length > 0) {
    var fiber = queue.shift();
    if (!fiber) continue;
    var name = fiber.type && (fiber.type.displayName || fiber.type.name || "");
    if (name === "CharacteristicBoolean") {
      var props = fiber.memoizedProps || {};
      if (props.charUuid === "TARGET-UUID-HERE") {
        target = props;
        break;
      }
    }
    if (fiber.child) queue.push(fiber.child);
    if (fiber.sibling) queue.push(fiber.sibling);
  }

  // onWrite signature: (charUuid, encodedNewValue, encodedPreviousValue)
  // true  → 'AQ=='  (btoa of byte 0x01)
  // false → 'AA=='  (btoa of byte 0x00)
  target.onWrite("TARGET-UUID-HERE", "AQ==", target.charInfo.value);
  return "done";
})();
```

**UUID scheme for animation boolean characteristics:**

- Service UUID: `BT_ANIMATION_SERVICE_UUID(anim_id)` = `12345678-1234-5678-{anim_id<<8:04x}-56789abd0000`
- `Animation::Rainbow = 5` → service `0500`, Is Active (3rd char, index 2) → `12345678-1234-5678-0500-56789abd0002`
- Find current value first: iterate `CharacteristicBoolean` fibers, read `charInfo.value` (`AA==`=false, `AQ==`=true)
- Animation enum values are in `fw/src/animations/animation_types.h`
- **Extension animations** (`fw/src/extensions/`, ids `0x40 + slot`) → service groups `4000`, `4100`, … Their "Is Active" uses the FIXED shared UUID `...-bbbb-...0000` — the same literal UUID appears in every animation service, so **always disambiguate by `charInfo.characteristic.serviceUUID`, never by `charUuid` alone**. Their param characteristics use auto UUIDs `...-{group}-56789abd0001/0002/...` in manifest declaration order (ids start at 1).

**Per-CPF fiber component names** (all take the same `onWrite(charUuid, encodedNewValue, encodedPreviousValue)` prop, so the CharacteristicBoolean recipe above works for every type): `CharacteristicBoolean`, `CharacteristicUint32` (4-byte LE, e.g. 50 = `MgAAAA==`), `CharacteristicColor` (4 bytes `b,g,r,mode` — mode 0 = static, see Color Encoding), `CharacteristicUtf8` (write with `btoa("text")`). The fiber walk matches `fiber.type.displayName || fiber.type.name` — these components are named function exports (`components/characteristic-*.tsx`) with no explicit `displayName`, so it's the function *name* that matches; nothing automated enforces this, so keep those component function names stable as a convention (renaming one silently breaks these recipes). These fibers only exist while the screen that renders them is mounted — Is Active toggles live on the Controls list, per-param characteristics only on that animation's detail page (navigate there first or the walk returns "not found").

### BLE Optimistic UI and Notification Behaviour

The app uses optimistic updates: the UI reflects the new value immediately, then reverts if the BLE write returns an error. The optimistic value is applied **synchronously before `await writeWithResponse(...)`** in `writeToCharacteristic`/`writeServiceCharacteristic` (`context/bluetooth-context.tsx`), batched into the same render as `isUpdateInProgress=true`. On rejection the `catch` reverts to the captured previous value **compare-and-swap style** — only if the current value is still the one we optimistically wrote — so a device notification (or an overlapping write) that landed during the in-flight write isn't clobbered by a stale revert. (It used to run in the write promise's `.then()` — that ordering left a render where a controlled input like the "Is Active" `Switch` still showed its old value while the write was in flight, which caused the toggle-flicker fixed in issue #91.) After a successful write, the **device sends back BLE notifications** with its actual characteristic values. These notifications go through `updateCharValue()` in the Bluetooth context and override the optimistic state with whatever the device actually holds.

Practical implications:

- A write that succeeds in the app may still show a different value if the device sends a notification with a different (e.g., clamped or normalised) value shortly after.
- Characteristic values that are not persisted in NVS reset to firmware defaults after a device reboot.
- **Firmware must refuse unacceptable writes with an ATT error, not "success + corrective notify".** Firmware should reject bad writes with an ATT **error** (triggering the app's catch-and-revert), never accept-then-notify-a-different-value — see the matching rule in `fw/CLAUDE.md` (`bt_service_cpp.h` section). Historically this was mandatory because the optimistic update ran in the write's `.then()` and would clobber any notification that arrived before the write response resolved (a corrective notify was silently lost — hardware-verified). Since the issue-#91 fix moved the optimistic update to *before* the `await`, a notification arriving during the write window now lands after it and wins, so a corrective notify would no longer be silently lost — but rejecting with an ATT error remains the required contract (it's what drives the revert, and it avoids a visible flash-then-correct). Notifications remain the mechanism for device-originated changes that happen after the write completes (e.g. an extension sandbox fault flipping Is Active off).

### Verifying a write/notify round-trip — don't trust a single "it updated" observation

A characteristic whose write-value and notified/stored value differ (e.g. any dropdown-list characteristic, see [components/characteristic-dropdown.tsx](components/characteristic-dropdown.tsx)) is easy to mis-verify, because several distinct bugs all produce the _same_ surface symptom: "I picked an option and the UI showed the new value." That observation alone does not distinguish:

- a correct write + correct notify (the real success case),
- an optimistic update that clobbers the real value before the (possibly failed) notify arrives,
- a no-op: the option tapped happened to match what the UI already (possibly stale) believed was selected, so no write was even sent,
- a notify that silently failed (e.g. exceeded the negotiated MTU — see the Known Issues entry above) while the UI happened to already show the right value from a stale read.

What actually caught the MTU/notify bugs in this codebase: reopening the picker afterward to confirm _all_ options are still listed (not just the one that appeared selected), and cross-checking the characteristic's value against the firmware's own source of truth immediately after the write (the `glim` shell command, via the `mcp__serial__*` tools) — not a different/unrelated characteristic. When verifying any BLE write, always do both before calling it confirmed.
