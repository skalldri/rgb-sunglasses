# Architecture Review

## Summary

This review covers the RGB Sunglasses app codebase with a focus on long-term maintainability.
The overall design direction is sound: metadata-driven UI, centralized BLE state, and clean separation of the SMP protocol layer are solid foundations.
The suggestions below are ordered roughly by impact, with the most impactful structural changes first.

---

## 1. Extract Connection Logic from the Component Layer

**File:** `components/bluetooth-device-list-item.tsx`

**Problem:** The `BluetoothDeviceListItem` component is ~230 lines and contains the entire BLE connection lifecycle: service discovery, descriptor reading, characteristic value reads, monitor subscription setup, and disconnect listener registration. This is the single largest maintainability risk in the codebase.

- It mixes UI rendering with complex async BLE orchestration.
- It makes the connection flow untestable without rendering a component.
- Any change to the connection handshake risks breaking the list item UI and vice versa.
- The disconnect handler references `selectedDevice` from a stale closure (line 187), which is a latent bug when the context value changes between connection and disconnection.

**Suggestion:** Extract the connection orchestration into a service or hook, for example `services/ble-connection.ts` or `hooks/use-ble-connection.ts`. This module would own:

- `connectToDevice(macAddress)` → returns a `BluetoothContextDevice`
- `disconnectFromDevice(macAddress)` → cleans up subscriptions and clears state
- Monitor subscription setup and teardown
- Disconnect listener registration

The component would call the hook and remain a thin UI shell (~30 lines) that shows device name, MAC, and a connect/disconnect button.

**Priority:** High — this is the change with the single biggest improvement to testability and readability.

---

## 2. Flatten the Nested Characteristic State Shape

**File:** `context/bluetooth-context.tsx`

**Problem:** The `characteristicsByService` structure is `Record<string, Record<string, CharacteristicInfo>>`, a doubly-nested map keyed by service UUID then characteristic UUID. Every operation that touches a single characteristic must:

1. Iterate or search all service keys to find which service owns the characteristic.
2. Perform a deep-clone spread across three levels of nesting to produce a new state object.

This causes:

- Verbose, error-prone update functions (`updateCharValue`, `setCharUpdateInProgress` are nearly identical 20-line functions).
- Unnecessary re-renders: any single characteristic write replaces the entire `selectedDevice` object, re-rendering every consumer.
- The `findServiceUuidForChar` helper existing at all is a code smell — it shouldn't be needed.

**Suggestion:** Add a flat `Record<string, CharacteristicInfo>` keyed by characteristic UUID alongside (or instead of) the nested structure. The service grouping can be a separate lightweight map (`Record<string, string[]>` of service UUID to characteristic UUIDs) used only for rendering order in device-state.tsx.

This eliminates the search-then-spread pattern and makes single-characteristic updates O(1) and one level of nesting deep.

**Priority:** High — reduces complexity in context, device-state, and any future consumers.

---

## 3. Remove Inline Styles from Device State and List Item

**Files:** `app/(tabs)/device-state.tsx`, `components/bluetooth-device-list-item.tsx`

**Problem:** Both files use extensive inline `style={{ ... }}` objects inside JSX. In device-state.tsx, the same text input style is duplicated across the UTF8 and UINT32 branches (lines 164-170 and 196-202). In the list item, layout styles are defined inline in the render return.

Inline styles:

- Defeat React Native's style deduplication (each render creates new objects).
- Make visual consistency harder to maintain.
- Increase JSX noise and reduce readability.

**Suggestion:** Move styles to `StyleSheet.create()` at the bottom of each file, consistent with the pattern already used in other files (`color-picker-modal.tsx`, `firmware-update-modal.tsx`). For the duplicated text input style, define it once and reference it by name.

**Priority:** Medium — low risk, improves consistency and minor performance gains.

---

## 4. Deduplicate Error-Handling Boilerplate in McuMgrClient

**File:** `services/mcumgr.ts`

**Problem:** Every command method (`getImageState`, `setImageState`, `eraseImage`, `getSlotInfo`, `uploadImage`) repeats the same pattern:

```typescript
if (response.rc && response.rc !== 0) {
    throw new Error(`<command> error: ${response.rc}`);
}
if (response.err) {
    throw new Error(`<command> error: group=${response.err.group}, rc=${response.err.rc}`);
}
```

This appears six times across the file. Additionally, the `ImageSlot` mapping logic (lines 548-558 and 593-603) is duplicated verbatim between `getImageState` and `setImageState`.

**Suggestion:**

- Extract a `throwOnSmpError(response, label)` helper.
- Extract a `parseImageSlots(response)` helper.

This reduces the command methods to their essential differences (which SMP group/command, which payload, which timeout).

**Priority:** Medium — reduces noise and prevents copy-paste drift.

---

## 5. Type the `any` Payloads in McuMgrClient

**File:** `services/mcumgr.ts`

**Problem:** The `sendRequest` method accepts `payload: any` and returns `Promise<any>`. Every caller then accesses untyped response properties (`response.rc`, `response.err`, `response.off`, `response.images`). The CBOR encode/decode functions also use `any`.

This defeats TypeScript's safety guarantees in the highest-risk part of the codebase (firmware operations).

**Suggestion:** Define request/response types per command:

```typescript
interface ImageUploadRequest { off: number; data: Uint8Array; len?: number; image?: number; sha?: Uint8Array; }
interface ImageStateResponse { images: RawImageSlot[]; rc?: number; err?: SmpErrorInfo; }
```

Then create typed wrapper methods (or use generics on `sendRequest<TResponse>`) so callers get autocomplete and compile-time checking.

**Priority:** Medium — high-value safety improvement with moderate effort.

---

## 6. Address the `useEffect` Dependency Warning in Firmware Modal

**File:** `app/firmware-update-modal.tsx`

**Problem:** The initialization `useEffect` (line 74) lists `[selectedDevice, client]` in its dependency array but uses `initializedRef` to prevent re-execution. Including `client` as a dependency means the effect's cleanup function captures a potentially stale `client` reference — the cleanup runs `client.destroy()` on the value of `client` at the time the effect last ran, not the current value.

Additionally, `setSelectedDevice` is called inside the effect to store the `mcuMgrClient` on the context (line 94), which mutates `selectedDevice` and could theoretically re-trigger the effect (guarded by the ref, but still a confusing dependency chain).

**Suggestion:** Remove `client` from the dependency array (the ref guard makes it safe, but the array is misleading). Better yet, move the client lifecycle into a dedicated `useMcuMgrClient(device)` hook that returns the client and handles init/destroy internally. This keeps the modal focused on UI orchestration.

**Priority:** Medium — reduces confusion and prevents future dependency-array bugs.

---

## 7. Eliminate the Unused `modal.tsx` Screen

**File:** `app/modal.tsx`

**Problem:** This is the Expo template's placeholder modal. It renders "This is a modal" with a link back to home. It is registered in `_layout.tsx` but never navigated to from anywhere in the app. It adds dead code and a wasted route entry.

**Suggestion:** Delete `app/modal.tsx` and remove its `Stack.Screen` entry from `_layout.tsx`.

**Priority:** Low — minimal effort, removes noise.

---

## 8. Remove Unused Dependencies

**File:** `package.json`

**Problem:** The `claude` package (line 26) is listed as a production dependency. This appears to be an accidental install — there are no imports of `claude` anywhere in the codebase. Unnecessary dependencies increase install time, bundle size, and supply-chain attack surface.

Several other Expo template dependencies may also be unused (`expo-symbols`, `expo-web-browser`, `expo-linking`), though these are lighter-weight and may be pulled in transitively.

**Suggestion:** Remove `claude` immediately. Audit the other Expo packages by searching for their imports; remove any that are not referenced.

**Priority:** Low for most, but the `claude` package should be removed promptly since it is clearly unintentional.

---

## 9. Improve the Permission Request UX

**File:** `hooks/use-ble.ts`

**Problem:** `requestPermissions()` returns a boolean but the caller in `bluetooth.tsx` (line 35) ignores the return value. If the user denies Bluetooth permissions, the scan silently starts and fails with no user feedback.

The permission dialog messages are also misleading: "Bluetooth Low Energy requires Location" is used for the `BLUETOOTH_SCAN` and `BLUETOOTH_CONNECT` permissions, which are not location permissions.

**Suggestion:**

- Check the return value and show an informative message if permissions are denied.
- Use accurate permission descriptions: "Required to discover and connect to nearby Bluetooth devices."

**Priority:** Low — UX polish, but easy to address.

---

## 10. Consider a BLE Connection State Machine

**Current state:** Connection lifecycle is managed through a combination of boolean flags (`canPress`, `isScanning`), nullable objects (`selectedDevice`), and callback closures. There is no explicit representation of states like "connecting", "discovering", "connected", "disconnecting", or "error".

**Problem:** This makes it difficult to:

- Show accurate loading states during the multi-second connection process.
- Handle edge cases (e.g., user navigates away during discovery).
- Prevent invalid transitions (e.g., disconnect during discovery).
- Debug lifecycle issues.

**Suggestion:** Define a union type for connection state:

```typescript
type ConnectionState =
  | { status: 'idle' }
  | { status: 'connecting'; macAddress: string }
  | { status: 'discovering'; device: Device }
  | { status: 'connected'; device: BluetoothContextDevice }
  | { status: 'disconnecting' }
  | { status: 'error'; message: string };
```

This can be managed with a simple reducer. The UI maps directly from the discriminated union, and invalid transitions are caught at compile time.

**Priority:** Low/Medium — higher effort, but significantly improves reliability and debuggability as the app grows.

---

## 11. Color Picker Performance

**File:** `app/color-picker-modal.tsx`

**Problem:** The color wheel renders 360 individual `<View>` elements (line 164) on every render. Each is a 2px-wide strip rotated to form a circle. This is a creative workaround for the lack of conic gradients in React Native, but 360 views is expensive — especially during touch-move events that update state on every frame.

**Suggestion:**

- Pre-render the wheel as a static image (generate once, cache as an asset or use `expo-image` with a pre-built gradient).
- Alternatively, use a library that provides native gradient support, or render the wheel with a Canvas/SVG approach.
- As a simpler quick fix, reduce the segment count (every 2° instead of every 1°) and memoize the wheel element array.

**Priority:** Low — the current approach works, but will cause noticeable jank on lower-end devices.

---

## 12. Harden the Scan Lifecycle

**File:** `app/(tabs)/bluetooth.tsx`

**Problem:**

- `startBluetoothScan` is an `async` function passed to `useFocusEffect`, which expects a synchronous cleanup function return. If the async scan setup throws, the cleanup function may not be registered.
- The `isDuplicateDevice` helper does a linear scan on every callback invocation (line 27). With frequent scan callbacks, this adds up.
- `bleManager.setLogLevel(LogLevel.Verbose)` is called on every focus event (line 37), which is unnecessary and noisy.

**Suggestion:**

- Wrap the async logic inside the callback and handle errors explicitly.
- Consider using a `Set` for deduplication instead of array search.
- Move `setLogLevel` to the module level or a one-time initialization.

**Priority:** Low — minor robustness improvements.

---

## Summary Table

| # | Area | Priority | Effort |
|---|------|----------|--------|
| 1 | Extract connection logic from component | High | Medium |
| 2 | Flatten characteristic state shape | High | Medium |
| 3 | Remove inline styles | Medium | Low |
| 4 | Deduplicate SMP error handling | Medium | Low |
| 5 | Type SMP payloads | Medium | Medium |
| 6 | Fix firmware modal useEffect deps | Medium | Low |
| 7 | Remove unused modal.tsx | Low | Trivial |
| 8 | Remove unused dependencies | Low | Trivial |
| 9 | Improve permission UX | Low | Low |
| 10 | BLE connection state machine | Low/Med | High |
| 11 | Color picker performance | Low | Medium |
| 12 | Harden scan lifecycle | Low | Low |

---

## What's Already Good

- **Metadata-driven UI** is a strong architectural choice that keeps app and firmware decoupled.
- **ble-value-codec.ts** is a clean extraction of encoding/decoding logic with good test coverage.
- **firmware-package.ts** properly separates parsing from UI.
- **mcumgr.ts** handles real protocol complexity (fragmentation, reassembly, stall detection) well.
- **Existing documentation** (architecture.md, roadmap.md, testing-roadmap.md) is unusually thorough for a project this size.
- **Test infrastructure** is already in place with sensible mocks and good coverage targets.
