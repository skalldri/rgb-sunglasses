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
import { ConnectionPriority } from 'react-native-ble-plx';

jest.mock('@/context/bluetooth-context', () => {
    const actual = jest.requireActual('@/context/bluetooth-context');
    return { ...actual, useBluetooth: jest.fn() };
});

// --- shared test fixture builders ---

function makeMockBluetooth() {
    return {
        selectedDevice: null as any,
        setSelectedDevice: jest.fn(),
        updateCharValue: jest.fn(),
        updateServiceCharacteristicValue: jest.fn(),
        setDiscoveryProgress: jest.fn(),
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
});
