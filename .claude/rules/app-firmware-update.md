---
paths:
  - "app/app/firmware-update/**"
  - "app/hooks/use-firmware-*.ts"
  - "app/hooks/use-mcumgr-client.ts"
  - "app/hooks/use-app-update-check.ts"
  - "app/context/firmware-update-context.tsx"
  - "app/context/mcumgr-client-context.tsx"
  - "app/services/mcumgr.ts"
  - "app/services/github-releases.ts"
  - "app/services/firmware-*.ts"
  - "fw/tools/dump_dfu_tlv.py"
---

# Firmware update in the app (MCUmgr / SMP, guided flow, release lookup)

Loads when you read the firmware-update screens, the MCUmgr client, or the release lookup. Running
an OTA end to end on hardware is `/ota-via-app`. Extension install/remove during an update:
`.claude/rules/extension-file-management.md`. The bootloader side (overwrite-only):
`.claude/rules/fw-sysbuild-mcuboot.md`.

## MCU Manager (`app/services/mcumgr.ts`)

SMP (Simple Management Protocol) for Zephyr firmware updates over BLE:

- CBOR-encoded messages with 8-byte headers; sequence numbers; a response can span multiple BLE
  notifications and is reassembled by the SMP header's length field (there is no "more data" flag).
- The client requests a 512-byte MTU (400 default). Image upload chunks are sized from the
  negotiated MTU (MTU − 3 ATT header − 64 bytes of CBOR overhead); BLE writes are split at MTU − 3.
  `uploadFile` also reserves the file name, which it repeats in every packet.
- The guided flow is **upload → stage as permanent → reset → verify**
  (`setImageState(hash, true)` in `app/hooks/use-firmware-update-flow.ts`). Only the debug page and
  the test-only `performFirmwareUpdate` still stage "for test".
- `getOsInfo('i')` (OS Management group, `OsCmd.INFO`) reads the device's board name string.

## Nested stack + providers

`app/app/firmware-update/` holds `index` (landing), `flow` (the guided update), `debug` (the old
high-detail page) and `extensions`, wrapped by a `_layout.tsx` that mounts **`McuMgrClientProvider`
and `FirmwareUpdateProvider`**. The MCUmgr provider is why it is a group at all:
`McuMgrClient.initialize()` registers a `monitor()` on the SMP characteristic, and a pushed screen
does **not** unmount the one below it, so a per-screen `useMcuMgrClient` would put two clients on
one characteristic (two notification registrations against Android's 15-slot budget, two response
handlers racing). `FirmwareUpdateProvider` (`app/context/firmware-update-context.tsx`) owns the one
release lookup and a shared busy flag (so the debug page's reset/erase are disabled mid-upload).
Screens draw their own in-body headers so they stay renderable in unit tests without a navigator.

## Firmware update: what you can and cannot verify an installed image against

The guided flow (`app/app/firmware-update/flow.tsx` + `app/hooks/use-firmware-update-flow.ts`)
proves an update actually landed by comparing hashes. Getting the *right* hash is the whole trick,
and there are two wrong answers that both look plausible:

- **The zip manifest has no hash.** Verified against the published `fw-v2.1.0`
  `dfu_application_proto0.zip`: each `manifest.json` entry carries only `type`, `board`, `soc`,
  `load_address`, `image_index`, the two `slot_index` fields, `version`/`version_MCUBOOT`, `size`,
  `file`, `modtime`. Don't go looking for a digest there — and don't trust the `ManifestFile`
  interface as evidence either way; check a real artifact.
- **`sha256(whole .bin)` is NOT the value the device reports.** The file ends with the MCUboot TLV
  trailer, which contains the digest, so the digest cannot cover it. For `fw-v2.1.0`'s
  `fw.signed.bin` the real values are `IMAGE_TLV_SHA256 = eeacf0fa…` versus
  `sha256(file) = 9f5d7d3a…`. Verification built on the whole-file digest would fail every update.

The value the device reports as a slot's `hash` in `getImageState()` **is** the image's own
`IMAGE_TLV_SHA256` (type `0x10`) — hardware-confirmed: after uploading `fw-v2.1.0` the board
reported `eeacf0fa…` for slot 1, matching the TLV extracted from the file. So `parseImageSha256()`
(`app/services/mcumgr.ts`) reads that TLV out of the `.bin` and the flow verifies the post-reboot
active slot against it, which is end-to-end: it never trusts the device's account of what it
received. `fw/tools/dump_dfu_tlv.py` is the reference implementation and the cross-check.

- **Images are staged as permanent (`setImageState(hash, true)`), and there is no rollback to be
  had.** The bootloader is overwrite-only (`CONFIG_BOOT_UPGRADE_ONLY`), whose Kconfig help says it
  *"prevents the fallback recovery"*, and this SoC cannot support a swap mode. Hardware-confirmed:
  an image staged as `pending` came back `active confirmed` with nothing confirming it. **Do not
  "fix" this to `confirm=false` expecting MCUboot to revert a bad image — it cannot**, and a
  test-then-confirm sequence would be a permanently no-op extra step. A failed verification means
  the device is running the wrong firmware and needs re-flashing, not restarting.
- **Verification cannot use the same signal for both cores.** Measured on hardware with fw-v2.1.0:

  | image | file TLV | staged slot 1 | active slot 0 after install |
  |---|---|---|---|
  | 0 app core | `eeacf0fa…` | `eeacf0fa…` | `eeacf0fa…` — stable |
  | 1 net core | `e43ebfa1…` | `e43ebfa1…` | `4d4b2c28…` — changes |

  The app core's image is flashed into its own slot verbatim so its hash survives; the net-core
  image is a wrapper the app core unwraps over IPC, so its file TLV can never match post-install.
  Hashes are checked at staging for every image (proving the upload arrived intact) and after reboot
  for the app core only, plus a version check for both. **Version comparison must stop at the
  `+build` boundary** — a bare `startsWith` accepts a shorter running version
  (`'2.1.10+0'.startsWith('2.1.1')` is true), which would verify a failed update as success.
- **Sync extensions BEFORE the activating restart.** They live on the FAT disk and are read at boot,
  so syncing after the reboot needs a second reboot — and a device that reboots into new firmware
  with old-ABI extensions has them rejected by `scan_slot()`, so the animations silently vanish. The
  guided flow does this in `handleRestart` (`app/app/firmware-update/flow.tsx`); a sync failure there
  does not block the restart, since the images are already staged and extensions can be retried.
- **After an OTA stages an image, the first J-Link reflash boots the OTA'd image, not the one you
  just flashed** (`.claude/skills/flash-and-verify/references/jlink.md`). Check the boot banner's
  version before trusting any measurement taken after a reflash.

## GitHub-releases firmware lookup

`useFirmwareReleaseLookup()` (`app/hooks/use-firmware-release.ts`) is called **once**, from
`FirmwareUpdateProvider`; screens read it through `useFirmwareRelease()`. Once an MCUmgr client
connects it: (1) calls `client.getOsInfo('i')` to read the board name (e.g.
`rgb_sunglasses_proto0_nrf5340_cpuapp`); (2) derives the board revision (`'proto0' | 'dk' | null`) via
`extractBoardRevision()`; (3) calls `fetchLatestFirmwareRelease()` (GitHub REST, no auth —
`app/services/github-releases.ts`) and picks the asset whose filename contains the revision
(`findAssetForBoard()`); (4) compares `parseVersionFromTag()` against the active slot-0 image's
`version` from `getImageState()` via `compareVersions()`. A strictly-older device version shows an
update card on the landing screen (`app/app/firmware-update/index.tsx`, "Install Update"), which
passes a `FirmwareSource` descriptor to the flow; `app/services/firmware-source.ts`
(`loadPackageFromRelease`, using `expo-file-system/legacy`'s `createDownloadResumable` for progress)
downloads and parses it with `parseFirmwarePackageFromBase64()` — the same path a picked `.zip` takes
(`loadPackageFromFile`, which uses the newer `expo-file-system/next` `File` API — `next` has no resumable-download
primitive, hence the legacy import for releases). (The feature was originally built on the pre-monorepo app repo's
(`skalldri/rgb-sunglasses-app`) `auto-update` branch and ported back by re-deriving that diff.)

**The GitHub lookup is unauthenticated and rate-limited per IP (60 req/hr)** — fine for on-demand
checks from one device, but don't add polling or retry-on-mount without a token, and don't add a
second per-screen lookup (pushed screens never unmount, so each would multiply requests). The same
limit applies to the app self-update check (`app/hooks/use-app-update-check.ts`).
