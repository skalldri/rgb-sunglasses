# Roadmap

## Purpose

Track the React Native rebuild and feature expansion for the RGB sunglasses controller app.
This roadmap is implementation-focused and should be updated as milestones are completed or reprioritized.

## Current State

Completed baseline:

- BLE scanning and connect/disconnect flow
- Runtime GATT discovery (services/characteristics/descriptors)
- Metadata-driven control rendering in Device State
- Characteristic write flows for boolean, UTF-8, uint32, and custom color
- Notification monitoring for notifiable characteristics
- Firmware update modal with MCUmgr/SMP upload, slot info, erase, and reset actions

## Guiding Priorities

1. Keep app/firmware decoupling strong through metadata-driven UX.
2. Increase reliability of BLE session handling and firmware updates.
3. Improve operator UX for configuring animations/text/media on glasses.
4. Add enough automated coverage to protect encoding/protocol behavior.

## Milestones

## Milestone 1: Foundation Hardening (Near Term)

Focus: stabilize current capabilities before major feature growth.

Targets:

- Strengthen error handling around descriptor reads and missing metadata.
- Improve disconnect/reconnect recovery across navigation and modal transitions.
- Normalize write result UX (success/failure states and retry affordances).
- Resolve naming mismatches in firmware UI ("test" vs current confirm behavior).
- Add app-side guardrails for unsupported characteristic formats.

Exit criteria:

- Connect/disconnect lifecycle is predictable across repeated sessions.
- Device State remains usable when partial metadata is missing.
- Firmware modal labels and behavior are consistent.

## Milestone 2: Control Surface Expansion

Focus: expose richer device controls while preserving dynamic UI generation.

Targets:

- Add support for additional CPF/data types as firmware exposes them.
- Introduce grouped/sectioned control layout for larger service sets.
- Improve discoverability for advanced actions (for example, service-specific tools).
- Add optional "raw value" inspector for debugging characteristic payloads.

Exit criteria:

- New firmware characteristics require minimal app code changes.
- Power-user flows are possible without sacrificing normal usability.

## Milestone 3: Content and Animation Authoring

Focus: core user value for RGB slats sunglasses.

Targets:

- Text rendering configuration UI (message, speed, direction, style).
- Animation parameter controls (pattern, timing, intensity, color behavior).
- Preset management (save/load local profiles).
- Optional media-to-frame preprocessing pipeline hooks (if firmware supports).

Exit criteria:

- Users can configure and apply meaningful visual effects from the app.
- Presets reduce repetitive manual editing.

## Milestone 4: Firmware Ops Maturity

Focus: production-grade firmware management path.

Targets:

- Preflight validation of selected package against device/board metadata.
- Better progress/error telemetry and resumable strategy where feasible.
- Clear rollback/recovery guidance for failed updates.
- Multi-image UX clarity (ordering, mapping, and expected post-update state).

Exit criteria:

- Firmware update success/failure states are explicit and actionable.
- Update workflow is safe enough for frequent iterative hardware development.

## Milestone 5: Quality and Release Readiness

Focus: confidence and maintainability.

Targets:

- Unit tests for value encoding/decoding and parsing helpers.
- Protocol-level tests for SMP packet framing/fragmentation behavior.
- Basic end-to-end validation checklist for BLE + firmware flows.
- Documentation refresh and onboarding guide for contributors.

Exit criteria:

- Core BLE and firmware workflows have regression protection.
- New contributors can become productive quickly.

## Cross-Cutting Workstreams

- Observability: improve structured logging around BLE operations and firmware commands.
- Security: review permission prompts and sensitive operation gating.
- Performance: reduce unnecessary rerenders in large characteristic sets.
- UX polish: improve loading, error, and empty states in tab and modal screens.

## Suggested Backlog (Next 2-4 Weeks)

1. Fix firmware modal wording/behavior mismatch around test vs confirm.
2. Add robust fallback labels when CUD/CPF data is absent or invalid.
3. Add retry/cancel patterns for characteristic writes and firmware operations.
4. Add tests for uint32 and custom color encoding paths.
5. Add tests for `parseImageHeader`, SMP header build/parse, and response reassembly.
6. Add a lightweight "device capabilities" summary panel generated from discovered metadata.

## How To Maintain This Document

- Keep milestone status current (`planned`, `in progress`, `done`).
- Move shipped work from backlog into changelog/release notes.
- Revisit priorities whenever firmware capabilities change materially.
