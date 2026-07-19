import { act, renderHook } from '@testing-library/react-native';

import * as BleHook from '@/hooks/ble-manager';
import * as BleConnectionHook from '@/hooks/use-ble-connection';
import { useBleRestorationAdopt } from '@/hooks/use-ble-restoration';

jest.mock('@/hooks/ble-manager', () => ({
    subscribeRestoredPeripheral: jest.fn(() => () => {}),
}));

jest.mock('@/hooks/use-ble-connection', () => ({
    useBleConnection: jest.fn(),
}));

const RESTORED = { mac: 'PERIPHERAL-UUID-1', name: 'RGB Sunglasses A1B2' };

describe('useBleRestorationAdopt (issue #190)', () => {
    let startReconnectLoop: jest.Mock;
    let unsubscribe: jest.Mock;
    // The subscriber the hook registered, captured so tests can simulate the
    // async restore event landing after mount.
    let subscriber: ((p: typeof RESTORED) => void) | null;

    beforeEach(() => {
        jest.clearAllMocks();
        jest.spyOn(console, 'log').mockImplementation(() => {});
        startReconnectLoop = jest.fn(async () => undefined);
        (BleConnectionHook.useBleConnection as jest.Mock).mockReturnValue({ startReconnectLoop });
        subscriber = null;
        unsubscribe = jest.fn();
        (BleHook.subscribeRestoredPeripheral as jest.Mock).mockImplementation((cb: any) => {
            subscriber = cb;
            return unsubscribe;
        });
    });

    afterEach(() => {
        jest.restoreAllMocks();
    });

    it('nothing delivered: binds useBleConnection to empty args and never starts the loop', () => {
        renderHook(() => useBleRestorationAdopt());

        expect(BleHook.subscribeRestoredPeripheral).toHaveBeenCalledTimes(1);
        expect(BleConnectionHook.useBleConnection).toHaveBeenCalledWith('', '');
        expect(startReconnectLoop).not.toHaveBeenCalled();
    });

    it('delivery BEFORE mount (immediate callback from subscribe): binds and starts the loop once', () => {
        (BleHook.subscribeRestoredPeripheral as jest.Mock).mockImplementation((cb: any) => {
            cb(RESTORED);
            return unsubscribe;
        });

        renderHook(() => useBleRestorationAdopt());

        expect(BleConnectionHook.useBleConnection).toHaveBeenLastCalledWith(RESTORED.mac, RESTORED.name);
        expect(startReconnectLoop).toHaveBeenCalledTimes(1);
    });

    it('delivery AFTER mount (async restore event lands later): binds and starts the loop once', () => {
        renderHook(() => useBleRestorationAdopt());
        expect(startReconnectLoop).not.toHaveBeenCalled();

        act(() => { subscriber?.(RESTORED); });

        expect(BleConnectionHook.useBleConnection).toHaveBeenLastCalledWith(RESTORED.mac, RESTORED.name);
        expect(startReconnectLoop).toHaveBeenCalledTimes(1);
    });

    it('starts the loop with the closure bound to the restored device, not the empty-args one', () => {
        // The starter effect must use the render where useBleConnection was
        // already re-bound to (mac, name) - assert via a per-args closure.
        const loops: Record<string, jest.Mock> = {};
        (BleConnectionHook.useBleConnection as jest.Mock).mockImplementation((mac: string) => {
            loops[mac] = loops[mac] ?? jest.fn(async () => undefined);
            return { startReconnectLoop: loops[mac] };
        });

        renderHook(() => useBleRestorationAdopt());
        act(() => { subscriber?.(RESTORED); });

        expect(loops[RESTORED.mac]).toHaveBeenCalledTimes(1);
        expect(loops['']).not.toHaveBeenCalled();
    });

    it('one-shot: a duplicate delivery cannot start a second competing loop', () => {
        renderHook(() => useBleRestorationAdopt());
        act(() => { subscriber?.(RESTORED); });
        act(() => { subscriber?.({ ...RESTORED }); });
        expect(startReconnectLoop).toHaveBeenCalledTimes(1);
    });

    it('re-renders do not restart the loop or resubscribe', () => {
        const { rerender } = renderHook(() => useBleRestorationAdopt());
        act(() => { subscriber?.(RESTORED); });
        rerender(undefined);
        expect(startReconnectLoop).toHaveBeenCalledTimes(1);
        expect(BleHook.subscribeRestoredPeripheral).toHaveBeenCalledTimes(1);
    });

    it('unmount unsubscribes', () => {
        const { unmount } = renderHook(() => useBleRestorationAdopt());
        unmount();
        expect(unsubscribe).toHaveBeenCalledTimes(1);
    });
});
