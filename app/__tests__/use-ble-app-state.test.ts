import { act, renderHook } from '@testing-library/react-native';
import { AppState } from 'react-native';

import * as BluetoothContext from '@/context/bluetooth-context';
import { useBleAppStateVerify } from '@/hooks/use-ble-app-state';
import * as BleConnectionHook from '@/hooks/use-ble-connection';

jest.mock('@/context/bluetooth-context', () => {
    const actual = jest.requireActual('@/context/bluetooth-context');
    return { ...actual, useBluetooth: jest.fn() };
});

jest.mock('@/hooks/use-ble-connection', () => ({
    useBleConnection: jest.fn(),
}));

describe('useBleAppStateVerify', () => {
    let appStateHandler: ((state: string) => void) | null;
    let removeListener: jest.Mock;
    let verifyConnection: jest.Mock;

    beforeEach(() => {
        jest.clearAllMocks();
        appStateHandler = null;
        removeListener = jest.fn();
        jest.spyOn(AppState, 'addEventListener').mockImplementation(((_type: string, handler: any) => {
            appStateHandler = handler;
            return { remove: removeListener };
        }) as any);

        verifyConnection = jest.fn(async () => undefined);
        (BluetoothContext.useBluetooth as jest.Mock).mockReturnValue({
            selectedDevice: { mac: 'AA:BB:CC', name: 'Test Device' },
        });
        (BleConnectionHook.useBleConnection as jest.Mock).mockReturnValue({ verifyConnection });
    });

    afterEach(() => {
        jest.restoreAllMocks();
    });

    it('binds useBleConnection to the selected device and verifies on foregrounding only', () => {
        renderHook(() => useBleAppStateVerify());

        expect(BleConnectionHook.useBleConnection).toHaveBeenCalledWith('AA:BB:CC', 'Test Device');
        expect(AppState.addEventListener).toHaveBeenCalledWith('change', expect.any(Function));

        act(() => { appStateHandler?.('background'); });
        expect(verifyConnection).not.toHaveBeenCalled();

        act(() => { appStateHandler?.('active'); });
        expect(verifyConnection).toHaveBeenCalledTimes(1);
    });

    it('calls the LATEST verifyConnection after the selected device changes (no stale closure)', () => {
        const { rerender } = renderHook(() => useBleAppStateVerify());

        const laterVerify = jest.fn(async () => undefined);
        (BluetoothContext.useBluetooth as jest.Mock).mockReturnValue({
            selectedDevice: { mac: 'ZZ:ZZ:ZZ', name: 'Other' },
        });
        (BleConnectionHook.useBleConnection as jest.Mock).mockReturnValue({ verifyConnection: laterVerify });
        rerender(undefined);

        act(() => { appStateHandler?.('active'); });
        expect(laterVerify).toHaveBeenCalledTimes(1);
        expect(verifyConnection).not.toHaveBeenCalled();
        // The listener itself was registered exactly once.
        expect(AppState.addEventListener).toHaveBeenCalledTimes(1);
    });

    it('removes the AppState listener on unmount', () => {
        const { unmount } = renderHook(() => useBleAppStateVerify());
        unmount();
        expect(removeListener).toHaveBeenCalledTimes(1);
    });

    it('handles no selected device (mac falls back to empty string)', () => {
        (BluetoothContext.useBluetooth as jest.Mock).mockReturnValue({ selectedDevice: null });
        renderHook(() => useBleAppStateVerify());
        expect(BleConnectionHook.useBleConnection).toHaveBeenCalledWith('', '');
    });
});
