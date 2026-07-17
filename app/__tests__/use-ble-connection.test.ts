import { act, renderHook, waitFor } from '@testing-library/react-native';
import React from 'react';

import {
    BLE_GATT_CPF_FORMAT_DROPDOWN_LIST,
    BLE_GATT_CPF_FORMAT_UTF8S,
    UUID_CCC_DESCRIPTOR,
    UUID_CPF_DESCRIPTOR,
    UUID_CUD_DESCRIPTOR,
    UUID_IS_ACTIVE_CHARACTERISTIC,
} from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import * as BleHook from '@/hooks/ble-manager';
import { useBleConnection } from '@/hooks/use-ble-connection';
import { SMP_CHARACTERISTIC_UUID, SMP_SERVICE_UUID } from '@/services/mcumgr';
import { Platform } from 'react-native';
import { ConnectionPriority } from 'react-native-ble-plx';

jest.mock('@/context/bluetooth-context', () => {
    const actual = jest.requireActual('@/context/bluetooth-context');
    return { ...actual, useBluetooth: jest.fn() };
});

jest.mock('@/services/ble-foreground-service', () => ({
    startConnectionService: jest.fn(async () => undefined),
    stopConnectionService: jest.fn(async () => undefined),
    updateConnectionNotification: jest.fn(async () => undefined),
}));

// eslint-disable-next-line import/first
import * as FgService from '@/services/ble-foreground-service';

// --- shared test fixture builders ---

function makeMockBluetooth() {
    // setConnectingDevice/setReconnectingDevice mirror the real useState setters: they accept
    // either a value or a functional updater, and apply it to a tracked value so tests can
    // assert the resulting pin (the connect flow clears the pin compare-and-swap style via an
    // updater, so a plain jest.fn that ignored the function wouldn't reflect the real outcome).
    const state = { connectingDevice: null as any, reconnectingDevice: null as any };
    const setConnectingDevice = jest.fn((next: any) => {
        state.connectingDevice = typeof next === 'function' ? next(state.connectingDevice) : next;
    });
    const setReconnectingDevice = jest.fn((next: any) => {
        state.reconnectingDevice = typeof next === 'function' ? next(state.reconnectingDevice) : next;
    });
    return {
        selectedDevice: null as any,
        setSelectedDevice: jest.fn(),
        updateCharValue: jest.fn(),
        updateServiceCharacteristicValue: jest.fn(),
        setDiscoveryProgress: jest.fn(),
        get connectingDevice() { return state.connectingDevice; },
        setConnectingDevice,
        get reconnectingDevice() { return state.reconnectingDevice; },
        setReconnectingDevice,
        reconnectGeneration: { current: 0 },
        intentionalDisconnectRef: { current: false },
        connectPromises: { current: {} as Record<string, Promise<boolean>> },
        monitorSubscriptions: { current: [] as any[] },
        disconnectSubscription: { current: null as any },
    };
}

function makeCharacteristic(uuid: string, opts: { notifiable?: boolean; readValue?: string | null } = {}) {
    return {
        uuid,
        isNotifiable: opts.notifiable ?? false,
        read: jest.fn(async () => ({ value: opts.readValue ?? null })),
        monitor: jest.fn((cb: any) => ({ remove: jest.fn(), _cb: cb })),
    };
}

function makeService(uuid: string, chars: ReturnType<typeof makeCharacteristic>[], descriptors: Record<string, any[]> = {}) {
    return {
        uuid,
        descriptorsForCharacteristic: jest.fn(async (charUuid: string) => descriptors[charUuid] ?? []),
        _chars: chars,
    };
}

function makeDeviceConnection(services: ReturnType<typeof makeService>[]) {
    return {
        discoverAllServicesAndCharacteristics: jest.fn(async () => undefined),
        services: jest.fn(async () => services),
        characteristicsForService: jest.fn(async (serviceUuid: string) =>
            services.find(s => s.uuid === serviceUuid)?._chars ?? []
        ),
        requestConnectionPriority: jest.fn(async () => undefined),
        requestMTU: jest.fn(async () => undefined),
    };
}

// ---

describe('useBleConnection', () => {
    let ctx: ReturnType<typeof makeMockBluetooth>;

    beforeEach(() => {
        // bleManager is a module-level singleton (jest.setup.ts's
        // react-native-ble-plx mock) shared across every test in this file -
        // restoreAllMocks() (afterEach, below) only reverts jest.spyOn
        // overrides, it does not clear a jest.fn()'s accumulated .mock.calls,
        // so without this a later test's toHaveBeenCalledTimes() would count
        // calls left over from earlier tests too (same fix as
        // bluetooth-screen.test.tsx).
        jest.clearAllMocks();
        jest.spyOn(console, 'log').mockImplementation(() => {});
        jest.spyOn(console, 'error').mockImplementation(() => {});

        ctx = makeMockBluetooth();
        (BluetoothContext.useBluetooth as jest.Mock).mockReturnValue(ctx);
    });

    afterEach(() => {
        jest.restoreAllMocks();
    });

    // ------------------------------------------------------------------
    // connect() happy path
    // ------------------------------------------------------------------

    it('connect() calls bleManager.connectToDevice with the correct mac', async () => {
        const deviceConn = makeDeviceConnection([]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        // Barebones connect - link only. refreshGatt is intentionally NOT passed
        // (it makes Android auto-start discovery that races and hangs the MTU
        // exchange, see connect()'s sequencing comment), and requestMTU runs as its
        // own step afterwards rather than inline.
        expect(BleHook.bleManager.connectToDevice).toHaveBeenCalledWith('AA:BB:CC', {
            timeout: 60000,
        });
        // MTU is negotiated as a separate post-connect step.
        expect(deviceConn.requestMTU).toHaveBeenCalledWith(247);
    });

    it('connect() pins the device in the Connect list for the attempt, then releases it (issue #158)', async () => {
        const deviceConn = makeDeviceConnection([]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        // Pinned at the very start of the attempt (before any await) so the scan-derived Connect
        // list can't prune it once the board stops advertising mid-pair; released in finally.
        expect(ctx.setConnectingDevice).toHaveBeenNthCalledWith(1, { mac: 'AA:BB:CC', name: 'Test Device' });
        expect(ctx.connectingDevice).toBeNull();
    });

    it('connect() clears the pin compare-and-swap - only if it still points at this device (concurrent connects, issue #158)', async () => {
        const deviceConn = makeDeviceConnection([]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        // The release is a functional updater (not a bare null): a concurrent connect to another
        // board shares this single context slot, so a settling attempt must never null out a pin
        // that now points at that other in-flight device (which would un-pin it mid-pairing and
        // reintroduce #158 for it). Exercise the updater directly.
        const calls = (ctx.setConnectingDevice as jest.Mock).mock.calls;
        const release = calls[calls.length - 1][0];
        expect(typeof release).toBe('function');
        // Clears its own pin...
        expect(release({ mac: 'AA:BB:CC', name: 'Test Device' })).toBeNull();
        // ...but leaves a pin a concurrent connect to another board has since installed.
        expect(release({ mac: 'ZZ:ZZ:ZZ', name: 'Other Board' })).toEqual({ mac: 'ZZ:ZZ:ZZ', name: 'Other Board' });
        expect(release(null)).toBeNull();
    });

    it('connect() releases the Connect-list pin even when the attempt fails (issue #158)', async () => {
        (BleHook.bleManager.connectToDevice as jest.Mock)
            .mockRejectedValue(new Error('Operation was cancelled'));

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        // finally runs on the failure path too, so the pin never leaks past a failed attempt.
        expect(ctx.setConnectingDevice).toHaveBeenNthCalledWith(1, { mac: 'AA:BB:CC', name: 'Test Device' });
        expect(ctx.connectingDevice).toBeNull();
    });

    it('connect() retries connectToDevice once when the first attempt fails', async () => {
        const deviceConn = makeDeviceConnection([]);
        // First attempt: the hardware-observed hang-then-cancel (native link comes
        // up and encrypts fine via the stored bond, but the app's connectToDevice
        // promise never resolves and rejects "Operation was cancelled"). The retry
        // must attach to that already-established link.
        (BleHook.bleManager.connectToDevice as jest.Mock)
            .mockRejectedValueOnce(new Error('Operation was cancelled'))
            .mockResolvedValueOnce(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        expect(BleHook.bleManager.connectToDevice).toHaveBeenCalledTimes(2);
        // Both attempts are barebones (link only) - the differentiators are gone.
        expect((BleHook.bleManager.connectToDevice as jest.Mock).mock.calls[0][1])
            .toEqual({ timeout: 60000 });
        expect((BleHook.bleManager.connectToDevice as jest.Mock).mock.calls[1][1])
            .toEqual({ timeout: 60000 });
        // The failed attempt's half-open native GATT client was force-closed
        // BETWEEN the attempts (else the retry queues behind it and hangs too).
        expect(BleHook.bleManager.cancelDeviceConnection).toHaveBeenCalledTimes(1);
        expect(BleHook.bleManager.cancelDeviceConnection).toHaveBeenCalledWith('AA:BB:CC');
        // MTU is requested as a separate step after the successful attempt.
        expect(deviceConn.requestMTU).toHaveBeenCalledWith(247);
        // The retry's connection is used for the rest of the flow - discovery ran.
        expect(deviceConn.discoverAllServicesAndCharacteristics).toHaveBeenCalled();
    });

    it('connect() gives up and cleans up after both connect attempts fail', async () => {
        (BleHook.bleManager.connectToDevice as jest.Mock)
            .mockRejectedValue(new Error('Operation was cancelled'));

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        expect(BleHook.bleManager.connectToDevice).toHaveBeenCalledTimes(2);
        // Failure path: cancelDeviceConnection runs both between the attempts and
        // in the final catch, so the possibly-orphaned native link is dropped and
        // the board goes back to advertising.
        expect(BleHook.bleManager.cancelDeviceConnection).toHaveBeenCalledTimes(2);
        expect(BleHook.bleManager.cancelDeviceConnection).toHaveBeenCalledWith('AA:BB:CC');
        expect(result.current.isConnecting).toBe(false);
    });

    it('connect() requests a high connection priority before discovery', async () => {
        const deviceConn = makeDeviceConnection([]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        expect(deviceConn.requestConnectionPriority).toHaveBeenCalledWith(ConnectionPriority.High);
        expect(deviceConn.discoverAllServicesAndCharacteristics).toHaveBeenCalled();
    });

    it('connect() proceeds with discovery even if requestConnectionPriority fails', async () => {
        const deviceConn = makeDeviceConnection([]);
        (deviceConn.requestConnectionPriority as jest.Mock).mockRejectedValue(new Error('unsupported'));
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        expect(deviceConn.discoverAllServicesAndCharacteristics).toHaveBeenCalled();
        expect(ctx.setSelectedDevice).toHaveBeenCalled();
    });

    it('connect() parses CUD and CPF descriptors into CharacteristicInfo', async () => {
        const char = makeCharacteristic('char-1', { notifiable: false, readValue: btoa('hello') });
        const service = makeService('svc-1', [char], {
            'char-1': [
                {
                    uuid: UUID_CUD_DESCRIPTOR,
                    read: jest.fn(async () => ({ value: btoa('My Char') })),
                },
                {
                    uuid: UUID_CPF_DESCRIPTOR,
                    read: jest.fn(async () => ({
                        value: btoa(String.fromCharCode(BLE_GATT_CPF_FORMAT_UTF8S, 0, 0, 0, 0, 0, 0)),
                    })),
                },
            ],
        });
        const deviceConn = makeDeviceConnection([service]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        const payload = ctx.setSelectedDevice.mock.calls[0][0];
        expect(payload.characteristicsByService['svc-1']['char-1'].name).toBe('My Char');
        expect(payload.characteristicsByService['svc-1']['char-1'].cpfFormat).toBe(BLE_GATT_CPF_FORMAT_UTF8S);
        expect(payload.characteristicsByService['svc-1']['char-1'].value).toBe(btoa('hello'));
    });

    it('connect() populates both flat characteristics and nested characteristicsByService', async () => {
        const char = makeCharacteristic('char-1', { readValue: btoa('val') });
        const service = makeService('svc-1', [char]);
        const deviceConn = makeDeviceConnection([service]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        const payload = ctx.setSelectedDevice.mock.calls[0][0];
        expect(payload.characteristics['char-1']).toBeDefined();
        expect(payload.characteristicsByService['svc-1']['char-1']).toBeDefined();
        expect(payload.serviceCharacteristics['svc-1']).toEqual(['char-1']);
        // both maps should point to the same CharacteristicInfo object
        expect(payload.characteristics['char-1']).toBe(payload.characteristicsByService['svc-1']['char-1']);
    });

    it('connect() sets up a monitor for notifiable characteristics', async () => {
        const char = makeCharacteristic('char-notify', { notifiable: true, readValue: null });
        const service = makeService('svc-1', [char]);
        const deviceConn = makeDeviceConnection([service]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        expect(char.monitor).toHaveBeenCalledTimes(1);
        expect(ctx.monitorSubscriptions.current).toHaveLength(1);
    });

    it('connect() skips monitor setup for the SMP characteristic', async () => {
        const smpChar = makeCharacteristic(SMP_CHARACTERISTIC_UUID, { notifiable: true });
        const smpService = makeService(SMP_SERVICE_UUID, [smpChar]);
        const deviceConn = makeDeviceConnection([smpService]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        expect(smpChar.monitor).not.toHaveBeenCalled();
        expect(ctx.monitorSubscriptions.current).toHaveLength(0);
    });

    it('monitor callback calls updateCharValue when a notification arrives', async () => {
        const char = makeCharacteristic('char-notify', { notifiable: true });
        const service = makeService('svc-1', [char]);
        const deviceConn = makeDeviceConnection([service]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        const monitorCall = char.monitor.mock.calls[0];
        const monitorCallback = monitorCall[0];

        act(() => { monitorCallback(null, { value: btoa('new-value') }); });

        expect(ctx.updateCharValue).toHaveBeenCalledWith('char-notify', btoa('new-value'));
    });

    it('monitor callback re-reads dropdown-list characteristics instead of trusting the notified value', async () => {
        const char = makeCharacteristic('char-dropdown', {
            notifiable: true,
            readValue: btoa('Option B\nOption A'),
        });
        char.read = jest.fn(async () => ({ value: btoa('Option A\nOption B') }));
        const service = makeService('svc-1', [char], {
            'char-dropdown': [
                {
                    uuid: UUID_CPF_DESCRIPTOR,
                    read: jest.fn(async () => ({
                        value: btoa(String.fromCharCode(BLE_GATT_CPF_FORMAT_DROPDOWN_LIST, 0, 0, 0, 0, 0, 0)),
                    })),
                },
            ],
        });
        const deviceConn = makeDeviceConnection([service]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        const monitorCallback = char.monitor.mock.calls[0][0];

        // The notified value is just the bare new selection (no separators) - not the full
        // canonical list. The callback must re-read rather than pass this straight through.
        // The real react-native-ble-plx monitor callback passes back the same Characteristic
        // instance (with .read() available), just with an updated .value - mirror that here.
        await act(async () => { await monitorCallback(null, { ...char, value: btoa('Option A') }); });

        expect(char.read).toHaveBeenCalled();
        await waitFor(() => {
            expect(ctx.updateCharValue).toHaveBeenCalledWith('char-dropdown', btoa('Option A\nOption B'));
        });
        expect(ctx.updateCharValue).not.toHaveBeenCalledWith('char-dropdown', btoa('Option A'));
    });

    it('excludes UUID_IS_ACTIVE_CHARACTERISTIC from the flat map and routes its notifications through updateServiceCharacteristicValue', async () => {
        const isActiveChar = makeCharacteristic(UUID_IS_ACTIVE_CHARACTERISTIC, { notifiable: true, readValue: btoa('\x00') });
        const service = makeService('svc-1', [isActiveChar]);
        const deviceConn = makeDeviceConnection([service]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        const payload = ctx.setSelectedDevice.mock.calls[0][0];
        expect(payload.characteristics[UUID_IS_ACTIVE_CHARACTERISTIC]).toBeUndefined();
        expect(payload.serviceCharacteristics['svc-1']).toEqual([]);
        expect(payload.characteristicsByService['svc-1'][UUID_IS_ACTIVE_CHARACTERISTIC]).toBeDefined();

        const monitorCallback = isActiveChar.monitor.mock.calls[0][0];
        act(() => { monitorCallback(null, { value: btoa('\x01') }); });

        expect(ctx.updateServiceCharacteristicValue).toHaveBeenCalledWith('svc-1', UUID_IS_ACTIVE_CHARACTERISTIC, btoa('\x01'));
        expect(ctx.updateCharValue).not.toHaveBeenCalledWith(UUID_IS_ACTIVE_CHARACTERISTIC, btoa('\x01'));
    });

    it('connect() registers a disconnect listener', async () => {
        const deviceConn = makeDeviceConnection([]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        expect(BleHook.bleManager.onDeviceDisconnected).toHaveBeenCalledWith('AA:BB:CC', expect.any(Function));
        expect(ctx.disconnectSubscription.current).not.toBeNull();
    });

    // ------------------------------------------------------------------
    // disconnect listener
    // ------------------------------------------------------------------

    it('disconnect listener cleans up monitors and calls setSelectedDevice(null)', async () => {
        const monitorRemove = jest.fn();
        const char = makeCharacteristic('char-notify', { notifiable: true });
        char.monitor.mockReturnValue({ remove: monitorRemove, _cb: null });
        const service = makeService('svc-1', [char]);
        const deviceConn = makeDeviceConnection([service]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        let disconnectCallback: ((error: any, device: any) => void) | null = null;
        (BleHook.bleManager.onDeviceDisconnected as jest.Mock).mockImplementation(
            (_mac: string, cb: any) => {
                disconnectCallback = cb;
                return { remove: jest.fn() };
            }
        );

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        // The unexpected disconnect below starts the auto-reconnect loop (issue #124);
        // park its connect attempt on a never-resolving pending promise so this test's
        // assertions aren't raced by a mock reconnect completing.
        (BleHook.bleManager.connectToDevice as jest.Mock).mockReturnValue(new Promise(() => {}));

        act(() => { disconnectCallback?.(null, { id: 'AA:BB:CC' }); });

        expect(monitorRemove).toHaveBeenCalledTimes(1);
        expect(ctx.monitorSubscriptions.current).toHaveLength(0);
        expect(ctx.setSelectedDevice).toHaveBeenLastCalledWith(null);
        expect(ctx.disconnectSubscription.current).toBeNull();
    });

    it('disconnect listener uses selectedDeviceRef to destroy mcuMgrClient, not a stale closure', async () => {
        const deviceConn = makeDeviceConnection([]);
        (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

        let disconnectCallback: ((error: any, device: any) => void) | null = null;
        (BleHook.bleManager.onDeviceDisconnected as jest.Mock).mockImplementation(
            (_mac: string, cb: any) => {
                disconnectCallback = cb;
                return { remove: jest.fn() };
            }
        );

        const mockDestroy = jest.fn();

        // After connect(), simulate the context updating selectedDevice to include a mcuMgrClient.
        // The hook's selectedDeviceRef should pick this up on the next render.
        const { result, rerender } = renderHook(
            ({ sel }: { sel: any }) => {
                (BluetoothContext.useBluetooth as jest.Mock).mockReturnValue({ ...ctx, selectedDevice: sel });
                return useBleConnection('AA:BB:CC', 'Test Device');
            },
            // `as any` so the rerender below can pass a device object — TS infers the
            // props type from this initial value, not the hook param's annotation.
            { initialProps: { sel: null as any } }
        );

        await act(async () => { await result.current.connect(); });

        // Now simulate selectedDevice being updated with a mcuMgrClient
        const deviceWithClient = { mac: 'AA:BB:CC', mcuMgrClient: { destroy: mockDestroy } };
        rerender({ sel: deviceWithClient });

        // Park the auto-reconnect the unexpected disconnect will start (issue #124).
        (BleHook.bleManager.connectToDevice as jest.Mock).mockReturnValue(new Promise(() => {}));

        act(() => { disconnectCallback?.(null, { id: 'AA:BB:CC' }); });

        expect(mockDestroy).toHaveBeenCalledTimes(1);
    });

    // ------------------------------------------------------------------
    // disconnect()
    // ------------------------------------------------------------------

    it('disconnect() removes subscriptions and calls cancelDeviceConnection', async () => {
        const removeDisconnect = jest.fn();
        const sub1Remove = jest.fn();
        ctx.monitorSubscriptions.current = [{ remove: sub1Remove }];
        ctx.disconnectSubscription.current = { remove: removeDisconnect };

        (BleHook.bleManager.cancelDeviceConnection as jest.Mock).mockResolvedValue(undefined);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.disconnect(); });

        expect(removeDisconnect).toHaveBeenCalledTimes(1);
        expect(sub1Remove).toHaveBeenCalledTimes(1);
        expect(ctx.monitorSubscriptions.current).toHaveLength(0);
        expect(BleHook.bleManager.cancelDeviceConnection).toHaveBeenCalledWith('AA:BB:CC');
        expect(ctx.setSelectedDevice).toHaveBeenCalledWith(null);
    });

    // ------------------------------------------------------------------
    // isConnecting state
    // ------------------------------------------------------------------

    it('isConnecting is false initially, true during connect, false after', async () => {
        let resolveConnect!: () => void;
        const connectPromise = new Promise<any>(res => { resolveConnect = () => res(makeDeviceConnection([])); });
        (BleHook.bleManager.connectToDevice as jest.Mock).mockReturnValue(connectPromise);

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        expect(result.current.isConnecting).toBe(false);

        act(() => { result.current.connect(); });

        await waitFor(() => expect(result.current.isConnecting).toBe(true));

        await act(async () => { resolveConnect(); });

        await waitFor(() => expect(result.current.isConnecting).toBe(false));
    });

    it('connect() failure resets isConnecting to false', async () => {
        (BleHook.bleManager.connectToDevice as jest.Mock).mockRejectedValue(new Error('BLE error'));

        const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));

        await act(async () => { await result.current.connect(); });

        expect(result.current.isConnecting).toBe(false);
    });

    // ------------------------------------------------------------------
    // auto-reconnect (issue #124)
    // ------------------------------------------------------------------

    describe('auto-reconnect', () => {
        // The reconnect link step's expected connectToDevice options are
        // platform-dependent: Android uses an autoConnect pending connection,
        // iOS a timeout-less direct connect (natively never-expiring).
        const pendingConnectOptions = Platform.OS === 'android' ? { autoConnect: true } : {};

        // Connects, captures the registered disconnect callback, then fires an
        // unexpected disconnect with the NEXT connectToDevice call resolving to
        // `reconnectResult` (default: a fresh device connection). Returns helpers.
        async function connectThenDrop(reconnectImpl?: (mock: jest.Mock) => void) {
            const deviceConn = makeDeviceConnection([]);
            (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(deviceConn);

            let disconnectCallback: ((error: any, device: any) => void) | null = null;
            (BleHook.bleManager.onDeviceDisconnected as jest.Mock).mockImplementation(
                (_mac: string, cb: any) => {
                    disconnectCallback = cb;
                    return { remove: jest.fn() };
                }
            );

            const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));
            await act(async () => { await result.current.connect(); });
            (BleHook.bleManager.connectToDevice as jest.Mock).mockClear();

            if (reconnectImpl) {
                reconnectImpl(BleHook.bleManager.connectToDevice as jest.Mock);
            } else {
                (BleHook.bleManager.connectToDevice as jest.Mock).mockResolvedValue(makeDeviceConnection([]));
            }

            return { result, fireDisconnect: () => disconnectCallback?.(null, { id: 'AA:BB:CC' }) };
        }

        it('an unexpected disconnect starts a reconnect with a pending (no-timeout) connect', async () => {
            const { fireDisconnect } = await connectThenDrop();

            await act(async () => { fireDisconnect(); });

            // Reconnecting state was pinned for the row UI...
            expect(ctx.setReconnectingDevice).toHaveBeenCalledWith({ mac: 'AA:BB:CC', name: 'Test Device' });
            await waitFor(() => {
                // ...and the reconnect attempt used the pending-connect options - crucially
                // NO timeout (a timeout cancels a pending connect).
                expect(BleHook.bleManager.connectToDevice).toHaveBeenCalledWith('AA:BB:CC', pendingConnectOptions);
            });
            // Reconnect succeeded -> reconnecting state cleared (CAS updater semantics).
            await waitFor(() => { expect(ctx.reconnectingDevice).toBeNull(); });
            // The reconnected device was published back into context.
            expect(ctx.setSelectedDevice).toHaveBeenLastCalledWith(expect.objectContaining({ mac: 'AA:BB:CC' }));
        });

        it('does NOT reconnect when the disconnect was user-initiated (structural guard: disconnect() removes the listener first)', async () => {
            const removeDisconnect = jest.fn();
            ctx.disconnectSubscription.current = { remove: removeDisconnect };
            (BleHook.bleManager.cancelDeviceConnection as jest.Mock).mockResolvedValue(undefined);

            const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));
            await act(async () => { await result.current.disconnect(); });

            // The subscription was removed BEFORE cancelDeviceConnection, so the OS
            // disconnect event can never reach the auto-reconnect handler.
            expect(removeDisconnect).toHaveBeenCalledTimes(1);
            // And no reconnect state was ever pinned.
            expect(ctx.setReconnectingDevice).not.toHaveBeenCalledWith({ mac: 'AA:BB:CC', name: 'Test Device' });
            expect(ctx.reconnectingDevice).toBeNull();
        });

        it('retries with backoff after a failed attempt, then succeeds', async () => {
            jest.useFakeTimers();
            try {
                const { fireDisconnect } = await connectThenDrop(mock => {
                    mock
                        .mockRejectedValueOnce(new Error('gatt error'))
                        .mockResolvedValue(makeDeviceConnection([]));
                });

                await act(async () => { fireDisconnect(); });

                // First attempt fired and failed; the retry must NOT run before the 2s backoff.
                await act(async () => { await jest.advanceTimersByTimeAsync(0); });
                expect(BleHook.bleManager.connectToDevice).toHaveBeenCalledTimes(1);

                await act(async () => { await jest.advanceTimersByTimeAsync(2000); });
                await act(async () => { await jest.advanceTimersByTimeAsync(0); });

                expect(BleHook.bleManager.connectToDevice).toHaveBeenCalledTimes(2);
                expect(ctx.reconnectingDevice).toBeNull(); // second attempt succeeded
            } finally {
                jest.useRealTimers();
            }
        });

        it('hedges every 3rd attempt with a direct (timeout-bounded) connect', async () => {
            jest.useFakeTimers();
            try {
                const { fireDisconnect } = await connectThenDrop(mock => {
                    mock.mockRejectedValue(new Error('gatt error'));
                });

                await act(async () => { fireDisconnect(); });
                // Attempt 1 (pending) fails -> +2s -> attempt 2 (pending) fails -> +5s -> attempt 3 (direct).
                await act(async () => { await jest.advanceTimersByTimeAsync(2000); });
                await act(async () => { await jest.advanceTimersByTimeAsync(5000); });

                const calls = (BleHook.bleManager.connectToDevice as jest.Mock).mock.calls;
                expect(calls.length).toBeGreaterThanOrEqual(3);
                expect(calls[0][1]).toEqual(pendingConnectOptions);
                expect(calls[1][1]).toEqual(pendingConnectOptions);
                expect(calls[2][1]).toEqual({ timeout: 60000 });
            } finally {
                jest.useRealTimers();
            }
        });

        it('a user Connect tap mid-reconnect shares the in-flight attempt instead of colliding', async () => {
            let resolvePending!: (v: any) => void;
            const { result, fireDisconnect } = await connectThenDrop(mock => {
                mock.mockReturnValue(new Promise(res => { resolvePending = res; }));
            });

            await act(async () => { fireDisconnect(); });
            await waitFor(() => expect(BleHook.bleManager.connectToDevice).toHaveBeenCalledTimes(1));

            const genBefore = ctx.reconnectGeneration.current;
            let userResult: boolean | undefined;
            act(() => { result.current.connect().then(r => { userResult = r; }); });

            // No second connectToDevice, and the user tap did NOT cancel the loop.
            expect(BleHook.bleManager.connectToDevice).toHaveBeenCalledTimes(1);
            expect(ctx.reconnectGeneration.current).toBe(genBefore);

            await act(async () => { resolvePending(makeDeviceConnection([])); });
            await waitFor(() => expect(userResult).toBe(true));
        });

        it('cancelReconnect() stops the loop, clears the pin, and aborts the pending connect', async () => {
            const { result, fireDisconnect } = await connectThenDrop(mock => {
                mock.mockReturnValue(new Promise(() => {})); // pending forever
            });

            await act(async () => { fireDisconnect(); });
            await waitFor(() => expect(ctx.reconnectingDevice).toEqual({ mac: 'AA:BB:CC', name: 'Test Device' }));
            const genBefore = ctx.reconnectGeneration.current;

            act(() => { result.current.cancelReconnect(); });

            expect(ctx.reconnectGeneration.current).toBe(genBefore + 1);
            expect(ctx.reconnectingDevice).toBeNull();
            expect(BleHook.bleManager.cancelDeviceConnection).toHaveBeenCalledWith('AA:BB:CC');
        });

        it('foreground service wiring: start on initial connect, update on drop + reconnect, stop on user disconnect', async () => {
            const { result, fireDisconnect } = await connectThenDrop();

            // Initial (user-initiated) connect STARTED the service.
            expect(FgService.startConnectionService).toHaveBeenCalledWith('Test Device');

            await act(async () => { fireDisconnect(); });

            // The unexpected drop switched the notification to Reconnecting… (an
            // update, never a service start - background starts are banned).
            expect(FgService.updateConnectionNotification).toHaveBeenCalledWith('Reconnecting to Test Device…');
            // The reconnect success refreshed it back to Connected.
            await waitFor(() => {
                expect(FgService.updateConnectionNotification).toHaveBeenCalledWith('Connected to Test Device');
            });
            // A reconnect success never STARTS the service (only the initial connect may).
            expect(FgService.startConnectionService).toHaveBeenCalledTimes(1);

            // User disconnect stops it.
            await act(async () => { await result.current.disconnect(); });
            expect(FgService.stopConnectionService).toHaveBeenCalled();
        });

        it('verifyConnection() recovers a missed disconnect: cleanup + reconnect loop', async () => {
            ctx.selectedDevice = { mac: 'AA:BB:CC', name: 'Test Device', mcuMgrClient: { destroy: jest.fn() } };
            const monitorRemove = jest.fn();
            ctx.monitorSubscriptions.current = [{ remove: monitorRemove }];
            const removeDisconnect = jest.fn();
            ctx.disconnectSubscription.current = { remove: removeDisconnect };
            (BluetoothContext.useBluetooth as jest.Mock).mockReturnValue(ctx);

            (BleHook.bleManager.isDeviceConnected as jest.Mock).mockResolvedValue(false);
            // Park the reconnect the recovery starts.
            (BleHook.bleManager.connectToDevice as jest.Mock).mockReturnValue(new Promise(() => {}));

            const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));
            await act(async () => { await result.current.verifyConnection(); });

            expect(removeDisconnect).toHaveBeenCalledTimes(1);
            expect(monitorRemove).toHaveBeenCalledTimes(1);
            expect(ctx.selectedDevice.mcuMgrClient.destroy).toHaveBeenCalledTimes(1);
            expect(ctx.setSelectedDevice).toHaveBeenCalledWith(null);
            expect(ctx.setReconnectingDevice).toHaveBeenCalledWith({ mac: 'AA:BB:CC', name: 'Test Device' });
        });

        it('verifyConnection() is a no-op when the link is healthy or the device is not selected', async () => {
            // Healthy link
            ctx.selectedDevice = { mac: 'AA:BB:CC', name: 'Test Device' };
            (BluetoothContext.useBluetooth as jest.Mock).mockReturnValue(ctx);
            (BleHook.bleManager.isDeviceConnected as jest.Mock).mockResolvedValue(true);

            const { result } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));
            await act(async () => { await result.current.verifyConnection(); });
            expect(ctx.setSelectedDevice).not.toHaveBeenCalled();

            // Different device selected
            ctx.selectedDevice = { mac: 'ZZ:ZZ:ZZ', name: 'Other' };
            (BluetoothContext.useBluetooth as jest.Mock).mockReturnValue(ctx);
            const { result: result2 } = renderHook(() => useBleConnection('AA:BB:CC', 'Test Device'));
            await act(async () => { await result2.current.verifyConnection(); });
            expect(ctx.setSelectedDevice).not.toHaveBeenCalled();
            expect(ctx.setReconnectingDevice).not.toHaveBeenCalled();
        });

        it('a superseded pending connect that resolves later is aborted, not adopted', async () => {
            let resolvePending!: (v: any) => void;
            const { result, fireDisconnect } = await connectThenDrop(mock => {
                mock.mockReturnValue(new Promise(res => { resolvePending = res; }));
            });

            await act(async () => { fireDisconnect(); });
            await waitFor(() => expect(BleHook.bleManager.connectToDevice).toHaveBeenCalledTimes(1));

            act(() => { result.current.cancelReconnect(); });
            (ctx.setSelectedDevice as jest.Mock).mockClear();
            (BleHook.bleManager.cancelDeviceConnection as jest.Mock).mockClear();

            // The pending connect resolves AFTER cancellation - it must not be adopted.
            await act(async () => { resolvePending(makeDeviceConnection([])); });

            await waitFor(() => {
                expect(BleHook.bleManager.cancelDeviceConnection).toHaveBeenCalledWith('AA:BB:CC');
            });
            expect(ctx.setSelectedDevice).not.toHaveBeenCalledWith(expect.objectContaining({ mac: 'AA:BB:CC' }));
        });
    });
});
