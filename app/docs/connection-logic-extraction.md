# Plan: Extract Connection Logic from BluetoothDeviceListItem

## Problem

`components/bluetooth-device-list-item.tsx` is ~230 lines. Only ~40 of those lines are UI.
The remaining ~190 lines are BLE orchestration embedded directly in a button handler:

- `connectToDevice` → service discovery → descriptor reads → characteristic reads → state building
- `setSelectedDevice` with a fully-constructed `BluetoothContextDevice`
- Monitor subscription setup for every notifiable characteristic
- `onDeviceDisconnected` listener registration with cleanup

This makes the connection flow:
- **Untestable without rendering a component** — the current test must render `<BluetoothDeviceListItem>` and simulate a button press to test connect/disconnect logic
- **Fragile to UI changes** — any refactor of the list item risks the connection flow and vice versa
- **Carrying a latent stale-closure bug** — the disconnect listener references `selectedDevice` from the outer closure (line 196), which may not be current by the time the listener fires

---

## Proposed Architecture

Extract all BLE orchestration into a new hook: **`hooks/use-ble-connection.ts`**.

The component becomes a thin UI shell that calls the hook and renders based on its returned state.

```
hooks/use-ble-connection.ts        ← new: all BLE logic lives here
components/bluetooth-device-list-item.tsx  ← reduced to ~45 lines of pure UI
__tests__/use-ble-connection.test.ts       ← new: direct unit tests for the logic
__tests__/bluetooth-device-list-item.test.tsx  ← simplified: only tests UI binding
```

---

## Hook API

```typescript
// hooks/use-ble-connection.ts

interface UseBleConnectionResult {
  isConnecting: boolean;
  connect: () => Promise<void>;
  disconnect: () => Promise<void>;
}

export function useBleConnection(
  macAddress: string,
  deviceName: string
): UseBleConnectionResult
```

`macAddress` and `deviceName` are passed as hook arguments (they come from the component's
props and are fixed per instance). This avoids needing to thread them through every call.

---

## Hook Internals (narrative)

### State & refs

```typescript
const [isConnecting, setIsConnecting] = useState(false);

// Fix for stale-closure bug: keeps a current reference to selectedDevice
// so the disconnect callback always sees the live value, not a captured snapshot
const selectedDeviceRef = useRef<BluetoothContextDevice | null>(null);
selectedDeviceRef.current = selectedDevice;

// Tracks whether the component that owns this hook is still mounted.
// Guards against calling setIsConnecting after unmount (avoids React warning).
const isMountedRef = useRef(true);
useEffect(() => () => { isMountedRef.current = false; }, []);
```

### `connect()` — same logic as today, extracted verbatim, with improvements

```typescript
async function connect(): Promise<void> {
  setIsConnecting(true);
  try {
    const deviceConnection = await bleManager.connectToDevice(macAddress);
    await deviceConnection.discoverAllServicesAndCharacteristics();
    const services = await deviceConnection.services();

    // Build characteristicsByService, characteristics, serviceCharacteristics
    // (exact same discovery + descriptor-read loop as today)

    setSelectedDevice({ name, mac, device, services, characteristicsByService,
                        characteristics, serviceCharacteristics });

    // Set up monitors for notifiable chars (same as today, skip SMP)

    // Register disconnect listener — uses selectedDeviceRef instead of captured selectedDevice
    disconnectSubscription.current = bleManager.onDeviceDisconnected(macAddress, (err, device) => {
      if (device?.id === macAddress) {
        monitorSubscriptions.current.forEach(sub => sub.remove());
        monitorSubscriptions.current = [];

        // ✅ fixed: reads current value, not stale closure snapshot
        if (selectedDeviceRef.current?.mcuMgrClient) {
          selectedDeviceRef.current.mcuMgrClient.destroy();
        }

        setSelectedDevice(null);
        if (isMountedRef.current) setIsConnecting(false);
        disconnectSubscription.current = null;
      }
    });
  } catch (error) {
    console.error(`Connection failed for ${macAddress}:`, error);
  } finally {
    if (isMountedRef.current) setIsConnecting(false);
  }
}
```

**Note:** Errors during connection are caught and logged; `isConnecting` is reset to `false`
either way. The component just re-enables its button — no error propagation needed for now.

### `disconnect()` — same logic as today

```typescript
async function disconnect(): Promise<void> {
  setIsConnecting(true);
  try {
    disconnectSubscription.current?.remove();
    disconnectSubscription.current = null;

    monitorSubscriptions.current.forEach(sub => sub.remove());
    monitorSubscriptions.current = [];

    await bleManager.cancelDeviceConnection(macAddress);
    setSelectedDevice(null);
  } finally {
    if (isMountedRef.current) setIsConnecting(false);
  }
}
```

---

## Simplified Component

After the refactor, the component becomes:

```typescript
export default function BluetoothDeviceListItem({ deviceName, macAddress }: Props) {
  const { selectedDevice } = useBluetooth();
  const { isConnecting, connect, disconnect } = useBleConnection(macAddress, deviceName);
  const router = useRouter();

  const isSelected = selectedDevice?.mac === macAddress;

  return (
    <View style={styles.container}>
      <ThemedText style={styles.deviceName}>{deviceName}</ThemedText>
      <ThemedText style={styles.macAddress}>{macAddress}</ThemedText>
      <View style={styles.buttonContainer}>
        <Button
          title={isSelected ? "Disconnect" : "Connect"}
          disabled={isConnecting}
          onPress={async () => {
            if (isSelected) {
              await disconnect();
            } else {
              await connect();
              router.navigate('/(tabs)/device-state');
            }
          }}
        />
        {isConnecting && (
          <View style={styles.loadingOverlay}>
            <ActivityIndicator size="small" color="#fff" />
          </View>
        )}
      </View>
    </View>
  );
}
```

The component drops from ~230 lines to ~45 lines. The `StyleSheet` at the bottom stays unchanged.

---

## Bugs Fixed As a Side Effect

| Bug | How it's fixed |
|-----|---------------|
| Stale `selectedDevice` in disconnect listener (line 196) | `selectedDeviceRef.current` is used instead |
| No error handling in connect flow (button stays disabled on failure) | `try/finally` always resets `isConnecting` |
| `setIsConnecting` called on unmounted component (navigation race) | `isMountedRef` guard |

---

## Test Strategy

### `__tests__/use-ble-connection.test.ts` (new)

These tests use `renderHook` from `@testing-library/react-native` and mock `bleManager` and
`useBluetooth`. They test the logic directly without any component rendering:

| Test | What it covers |
|------|----------------|
| `connect() calls bleManager.connectToDevice` | Happy path connect |
| `connect() reads descriptors and builds correct CharacteristicInfo` | Descriptor + CUD/CPF parsing |
| `connect() calls setSelectedDevice with flat + nested maps` | State shape contract |
| `connect() skips SMP characteristic from monitors` | Monitor filter |
| `connect() registers disconnect listener` | Listener setup |
| `disconnect listener fires → cleans up monitors, destroys mcuMgrClient` | Disconnect callback |
| `disconnect listener fires → uses current selectedDeviceRef, not stale closure` | Stale closure fix |
| `disconnect() cleans up subs and calls cancelDeviceConnection` | Manual disconnect |
| `connect() failure → isConnecting resets to false` | Error handling |
| `isConnecting is true during connect, false after` | Loading state |

### `__tests__/bluetooth-device-list-item.test.tsx` (simplified)

The component test no longer needs to mock the entire BLE stack. It mocks `useBleConnection`
and verifies only UI behavior:

| Test | What it covers |
|------|----------------|
| Shows "Connect" / "Disconnect" based on `selectedDevice.mac` | Button label |
| `disabled` when `isConnecting` is true | Loading state |
| Calls `connect()` and then `router.navigate` on press | Wires hook to UI |
| Calls `disconnect()` on press (no navigate) | Wires hook to UI |
| Shows `ActivityIndicator` when `isConnecting` | Spinner visibility |

---

## Files Changed

| File | Change |
|------|--------|
| `hooks/use-ble-connection.ts` | **Create** — all BLE orchestration |
| `components/bluetooth-device-list-item.tsx` | **Replace** body with ~45-line UI shell |
| `__tests__/use-ble-connection.test.ts` | **Create** — ~10 focused unit tests |
| `__tests__/bluetooth-device-list-item.test.tsx` | **Replace** with ~5 lightweight UI tests |

No other files are touched. The public contract of `BluetoothContextDevice` and
`bluetooth-context.tsx` does not change.

---

## Out of Scope

These are adjacent improvements that are intentionally excluded to keep the diff focused:

- **Connection state machine** (review item #10) — would be a natural follow-up once the logic
  is in a hook. The hook's `isConnecting` boolean could evolve into a richer discriminated union.
- **SMP payload typing** (review item #5) — unrelated to this refactor.
- **Further reduction of `characteristicsByService`** — the nested structure is still populated
  for backwards compatibility with `device-state.tsx`'s render loop.
