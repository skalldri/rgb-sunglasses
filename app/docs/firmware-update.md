# Firmware Update

## Overview

Firmware updates are performed over BLE using MCUmgr (SMP protocol).
The app implements an internal `McuMgrClient` in `services/mcumgr.ts` and exposes the workflow through `app/firmware-update-modal.tsx`.

## BLE Endpoints

- SMP Service UUID: `8d53dc1d-1db7-4cd3-868b-8a527460aa84`
- SMP Characteristic UUID: `da2e7828-fbce-4e01-ae9e-261174997c48`

## Protocol Implementation

`McuMgrClient` currently handles:

- SMP header creation/parsing
- CBOR encode/decode for payloads
- request/response sequence management
- response reassembly for fragmented notifications
- MTU negotiation (`requestMTU(512)`) with fallback behavior
- BLE write chunking based on negotiated payload size

Supported command groups and operations include:

- Image group:
  - `STATE`
  - `UPLOAD`
  - `ERASE`
  - `SLOT_INFO`
- OS group:
  - `ECHO`
  - `RESET`
  - `MCUMGR_PARAMS`

## Firmware Package Format

The modal expects a `.zip` package with:

- `manifest.json`
- one or more firmware binary files referenced by manifest entries

Manifest fields used by the app include:

- `name`
- `files[]` with metadata like:
  - `file`
  - `image_index`
  - `slot_index_primary`
  - `slot_index_secondary`
  - `type`
  - `board`
  - `size`
  - `version` or `version_MCUBOOT` (when present)

The app loads the zip with `JSZip`, parses manifest JSON, and sorts images by numeric `image_index` before upload.

## Update Flow (Current Implementation)

1. Initialize `McuMgrClient` from the selected BLE device.
2. Fetch image state and optional slot info for display.
3. User selects firmware package zip.
4. App parses each image and attempts MCUboot header parsing for version display.
5. Upload each image chunk-by-chunk via SMP `UPLOAD`.
6. Refresh image state and locate uploaded image in secondary slot.
7. Set uploaded image state using its hash.
   Current modal implementation calls `setImageState(hash, true)` (confirm/permanent), even though some UI text still says "test".
8. Present completion or error status in modal UI.

Progress UI:

- Reports percent across all images in package, not only current file.

## Additional Device Actions

From the modal, users can also:

- refresh image/slot state
- reset device
- erase secondary slot (`slot 1`)
- mark slot 1 image for test from the "Current Images" section

## Encoding and Validation Notes

- Firmware upload uses SHA-256 hash of image data in the first upload packet.
- Response `rc` and grouped `err` fields are both checked and surfaced as errors.
- Image header parsing validates MCUboot magic `0x96f3b83d`.

## Operational Caveats

- Slot info command support may vary by device/firmware build.
- Device reset usually interrupts response flow and is treated as expected behavior.
- BLE instability, disconnects, or MTU differences may affect throughput.

## References in Code

- `services/mcumgr.ts`
- `app/firmware-update-modal.tsx`
- `context/bluetooth-context.tsx` (stores client for cleanup on disconnect)
