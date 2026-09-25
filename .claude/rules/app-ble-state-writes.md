---
paths:
  - "app/context/**"
  - "app/hooks/use-characteristic-editor.tsx"
  - "app/hooks/use-scoped-characteristic-monitors.ts"
  - "app/hooks/use-audio-*.ts"
  - "app/components/characteristic-*.tsx"
  - "app/components/battery-card.tsx"
  - "app/app/**/device-state/**"
  - "app/__tests__/**"
---

# App BLE state: optimistic writes, notifications, and the write→state loop

Loads when you read the Bluetooth context, characteristic components, device-state screens or
tests. The two rules for effects and deferred callbacks are in `app/CLAUDE.md` "What the device-free
loop CANNOT catch"; this file is the detail behind them. Firmware must reject bad writes with an ATT
error: `.claude/rules/ble-gatt-contract.md`.

## BLE Optimistic UI and Notification Behaviour

The app uses optimistic updates: the UI reflects the new value immediately, then reverts if the BLE
write returns an error. The optimistic value is applied **synchronously before
`await writeWithResponse(...)`** in `writeToCharacteristic`/`writeServiceCharacteristic`
(`app/context/bluetooth-context.tsx`), batched into the same render as `isUpdateInProgress=true`. On
rejection the `catch` reverts to the captured previous value **compare-and-swap style** — only if the
current value is still the one we optimistically wrote — so a device notification (or an overlapping
write) that landed during the in-flight write isn't clobbered by a stale revert. (It used to run in
the write promise's `.then()` — that ordering left a render where a controlled input like the "Is
Active" `Switch` still showed its old value while the write was in flight, which caused the
toggle-flicker fixed in issue #91.) After a successful write, the **device sends back notifications**
with its actual values; these go through `updateCharValue()` and override the optimistic state.

- A write that succeeds in the app may still show a different value if the device notifies a clamped
  or normalised value shortly after.
- Characteristic values that are not persisted reset to firmware defaults after a device reboot.
- Firmware must refuse unacceptable writes with an ATT **error** (drives the revert), never
  accept-then-notify; notifications are for device-originated changes after the write (e.g. an
  extension sandbox fault flipping Is Active off).
- **Clamp read-back** (`scheduleClampReadBack` in `bluetooth-context.tsx`): after a write to a
  characteristic the firmware may clamp, the app re-reads it in two passes
  (`CLAMP_READBACK_DELAYS_MS`, 150 and 1200 ms) — skipped for notifiable characteristics, whose
  notification already carries the truth.
- Verifying a write/notify round-trip on hardware (why "the UI showed the new value" proves little):
  `.claude/skills/submit-pr/references/device-verification.md`.

## What the device-free loop cannot catch — the PR #285 incidents (2026-08-05)

Jest suites mock `useBluetooth`, so `updateCharValue`/`updateServiceCharacteristicValue` are
`jest.fn()`s that **never produce a new context object**. That breaks the loop between "code
writes/reads a characteristic" and "context re-renders with fresh objects", so any bug living in that
loop is invisible to green tests no matter how many you add. Two such bugs shipped into a PR and
were caught only on hardware:

1. **Unbounded BLE read loop.** A `useFocusEffect(useCallback(fn, [charInfo, updateCharValue]))`
   re-read a characteristic, called `updateCharValue`, got back a new `charInfo` identity,
   invalidated the callback, and re-ran the effect — measured at **110 reads in 10 s**, continuously,
   saturating the GATT queue. Hence the rule: effect inputs come from `useRef`, and deps may only be
   values the effect's own writes cannot change — a plain string like `selectedDevice?.mac` is fine
   (it is how you re-arm on reconnect); anything context-derived (a `charInfo`, the device object, a
   context callback) closes the loop. `[]` is the common case, not the rule itself. Precedent:
   `app/components/battery-card.tsx` and `app/app/(tabs)/device-state/battery.tsx`.
2. **Deferred callback outliving its context.** A `setTimeout` read-back fired 150 ms after a write,
   by which time the characteristic could be torn down — `read()` then throws **synchronously**,
   which a bare `.catch()` does not handle, so a `TypeError` escaped as an unhandled error attributed
   to an unrelated test. Hence: in any fire-and-forget deferred BLE callback, optional-call
   (`read?.()`) AND wrap in `try/catch`. Precedent: `scheduleClampReadBack`.

Consequences:

- **A test asserting "the read happened" proves nothing about how many times it happens.** When adding
  a read-on-focus/interval path, also assert it does NOT re-fire: re-render with a *fresh* device
  object (mock `useBluetooth` with `mockImplementation`, not `mockReturnValue`, so each render returns
  new identities) and assert the read count is unchanged. Both precedent suites carry that test.
- **A deferred read must compare-and-swap before it applies.** By the time a delayed read-back
  resolves, a newer write or notification may already have set a fresher value; applying
  unconditionally snaps the control backwards. Patch through the function form and return `null`
  when the current value is no longer the one you wrote — the same guard the write-error revert uses.
  Precedent: `scheduleClampReadBack`, and the Active Animation fan-out in `app/hooks/use-ble-connection.ts`
  (which likewise skips services whose toggle did not change rather than rewriting every service).
- **Anything in this class needs a real device before merge.** `adb logcat | grep -c "Read from
  Characteristic"` over a fixed window is the cheap check — steady-state BLE traffic should be near
  zero when the UI is idle. `/submit-pr` step 5 mandates on-device verification for device↔app changes.

## Audio telemetry deliberately does NOT use `useScopedCharacteristicMonitors`

That hook's purpose is to sink notified values into the Bluetooth context, and `useBluetooth()`'s
value changes identity on every `selectedDevice` mutation — so sinking a 32 Hz stream through it would
re-render the whole app tree 32x/s and re-decode every characteristic each time.
`app/context/audio-telemetry-context.tsx` copies the hook's *structure* verbatim (refs-only effect
deps, a generation counter for the blur→focus race rxandroidble does not serialize, the
synchronous-throw-safe call wrapper, the cancel/disconnect error filter, the deferred re-arm for a
device that arrives after focus) and terminates in Reanimated shared values plus a 2 Hz
`useSyncExternalStore` summary instead. Do not "unify" the two: the divergence is the point.

Two bugs its tests found, both invisible by inspection: the generation guard must be the **first**
statement in the monitor callback (it originally sat below a `setStatus('streaming')`, so a late
callback after teardown left the pill reading LIVE over a dead stream while correctly discarding the
frame), and teardown must **notify** the summary/status listeners rather than assigning the refs (a
pushed screen does not unmount the one below it, so a still-mounted consumer otherwise renders the
last values it was told about).

Render-isolation tests are easy to write vacuously. `children` passed as a prop are immune by
construction because React reuses the element, so a provider deliberately broken to `setState` per
frame still passes a naive "did my child re-render" check; and notifying a `useSyncExternalStore`
listener without changing the snapshot cannot re-render anything. Assert the **context value
identity** is stable (kill it by dropping the EMPTY-deps `useMemo`) and that the **summary object
count** matches the tick count (kill it by moving `summarizeTelemetry` into the frame path), and check
both against those mutants.

**The telemetry wire format is cross-checked against the real firmware packer, not a spec reading.**
`fw/tools/gen_telemetry_vectors.cpp` compiles `fw/src/sound/audio_telemetry_codec.h` on the host,
calls `audio_telemetry_pack()` on known frames and emits golden bytes into
`app/__tests__/fixtures/audio-telemetry-vectors.json`; `audio-telemetry-vectors.test.ts` decodes them.
Regenerate both after any wire change — the command is in that test's header comment. This proves the
codec agrees; it does not prove the notify path, the MTU clamp or the subscription lifecycle.

## Keep characteristic component names stable

The fiber-walk recipes (`.claude/skills/drive-app/references/fiber-recipes.md`) match
`fiber.type.displayName || fiber.type.name`, and the `components/characteristic-*.tsx` components are
named function exports with no `displayName` — so the function *name* is what matches. Nothing
enforces it; renaming one silently breaks those recipes.
