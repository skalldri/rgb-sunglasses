# Testing Roadmap

## Purpose

Add broad and deep automated test coverage to reduce regressions and expose firmware/BLE bugs early, with priority on the MCUmgr firmware update path.

## What We Found During Inspection

Current state:

- No test files in the repository.
- No test runner or coverage scripts in `package.json`.
- High-risk logic is concentrated in:
  - `services/mcumgr.ts`
  - `app/firmware-update-modal.tsx`
  - `components/bluetooth-device-list-item.tsx`
  - `context/bluetooth-context.tsx`
  - `app/(tabs)/device-state.tsx`

## Testing Strategy

1. Start with pure and protocol logic where failures are expensive (firmware updates).
2. Add unit tests for state management and write flows.
3. Add component tests for critical user flows (connect, write, update).
4. Enforce coverage gates on high-risk modules first, then raise globally.

## Phase 0: Test Infrastructure (Week 1)

Goal: create a stable, fast local/CI test baseline.

Tasks:

- Add test stack:
  - `jest`
  - `jest-expo`
  - `@testing-library/react-native`
  - `@testing-library/jest-native`
  - `@types/jest`
- Add scripts:
  - `test`
  - `test:watch`
  - `test:coverage`
- Add Jest config with:
  - TypeScript + Expo support
  - `@/` alias mapping
  - setup file for common mocks/polyfills
- Add `jest.setup.ts` mocks/polyfills for:
  - `atob` / `btoa`
  - `expo-router`
  - `expo-document-picker`
  - `react-native-ble-plx`
  - `expo-file-system/next`
  - timers (`jest.useFakeTimers()` in targeted suites)

Exit criteria:

- `npm test` runs green on CI and local.
- One smoke test validates framework wiring.

## Phase 1: Firmware Protocol Unit Tests (Week 1-2, Highest Priority)

Target: `services/mcumgr.ts`

Coverage focus:

- `parseImageHeader`
  - valid MCUboot header
  - invalid magic
  - too-short buffers
  - expected version formatting
- `formatBytes` and `formatHash`
  - boundary values and formatting consistency
- `McuMgrClient.sendRequest` behavior (through public methods with mocked characteristic/device):
  - timeout path
  - chunk splitting for long payloads
  - CBOR payload decode path
  - error propagation
- response reassembly:
  - multi-fragment response completion
  - edge case where first fragment is short/incomplete
- `uploadImage`:
  - first packet includes `len`, `image`, and `sha`
  - subsequent packets omit first-packet-only fields
  - `response.off` handling and progress callback values
  - error handling on `rc` and `err`

High-value bug probes to codify as tests:

- very small negotiated MTU causing invalid chunk size math
- repeated `response.off` values causing non-progress loops
- sequence and response resolver behavior under overlapping calls

Exit criteria:

- `mcumgr.ts` has deep branch coverage and deterministic tests for upload/control paths.
- Known failure modes have explicit regression tests.

## Phase 2: Firmware Modal Logic Tests (Week 2)

Target: `app/firmware-update-modal.tsx`

Coverage focus:

- initialization states:
  - no selected device
  - init success
  - init failure
- package parsing:
  - cancelled picker flow
  - missing `manifest.json`
  - manifest with missing bin files
  - multi-image sort by `image_index`
- update flow:
  - progress calculation across multiple images
  - uploaded image lookup in returned image state
  - status and error messaging
- actions:
  - reset
  - erase slot
  - confirm/test action behavior

High-value bug probes:

- cleanup/destroy path on unmount after client initialization
- UI text/action mismatch around test vs confirm semantics

Exit criteria:

- Firmware modal behavior is covered for both happy path and critical failures.

## Phase 3: BLE State and Write Path Tests (Week 2-3)

Targets:

- `context/bluetooth-context.tsx`
- `app/(tabs)/device-state.tsx`

Coverage focus:

- context write behavior:
  - not-found characteristic returns `false`
  - `isUpdateInProgress` toggles correctly on success/failure
  - characteristic value updates only on successful write
- device-state value decoding/encoding:
  - boolean base64 conversion
  - uint32 little-endian encode/decode
  - UTF-8 handling and failure fallback behavior
  - write success/error status animation state transitions

High-value bug probes:

- decode fallback paths that can throw again in catch handlers
- uint32 conversion boundaries (0, max uint32, invalid input)

Exit criteria:

- Characteristic write/read transformations are deterministic and regression-tested.

## Phase 4: BLE Flow Component Tests (Week 3)

Targets:

- `app/(tabs)/bluetooth.tsx`
- `components/bluetooth-device-list-item.tsx`

Coverage focus:

- scan flow:
  - deduping discovered devices
  - filtering by expected device name
  - adding already-connected matching devices
- connect/disconnect flow:
  - service and descriptor discovery shaping context state
  - monitor subscription setup and teardown
  - disconnect listener cleanup behavior

High-value bug probes:

- permission denied path still triggering scan behavior
- connect failure leaving UI in disabled/loading state
- stale closure behavior in disconnect cleanup for MCUmgr client

Exit criteria:

- Core connect lifecycle is verified under success and failure conditions.

## Phase 5: Coverage Gates and CI Policy (Week 3-4)

Initial thresholds:

- `services/mcumgr.ts`: 90% lines, 85% branches
- `app/firmware-update-modal.tsx`: 80% lines, 75% branches
- global: 70% lines

After stabilization:

- raise global to 80% lines
- keep per-file strict gates on firmware/BLE-critical modules

Policy:

- PRs touching firmware/BLE logic must include or update tests.
- Failing coverage gates block merge.

## Recommended Small Refactors to Improve Testability

These are low-risk extractions that make unit tests easier and faster:

- Extract firmware package parsing from modal into `services/firmware-package.ts`.
- Extract uint32/boolean/base64 conversion helpers into `services/ble-value-codec.ts`.
- Extract SMP packet encode/decode helpers into `services/mcumgr-protocol.ts`.
- Wrap BLE manager interactions in thin adapters to simplify mocking.

## Suggested Execution Order

1. Land Phase 0 infra.
2. Land Phase 1 firmware protocol tests before feature work.
3. Land Phase 2 modal tests and fix issues found.
4. Land Phases 3-4 for BLE/session reliability.
5. Enforce Phase 5 coverage gates.

## Deliverables Checklist

- [x] Jest + Expo test setup committed
- [x] `mcumgr` unit suite committed
- [x] firmware modal unit/component suite committed
- [x] BLE context/device-state unit suite committed
- [x] Bluetooth flow component suite committed
- [ ] coverage thresholds enforced in CI
