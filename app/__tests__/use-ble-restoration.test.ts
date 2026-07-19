import { renderHook } from '@testing-library/react-native';

import * as BleHook from '@/hooks/ble-manager';
import * as BleConnectionHook from '@/hooks/use-ble-connection';
import { useBleRestorationAdopt } from '@/hooks/use-ble-restoration';

jest.mock('@/hooks/ble-manager', () => ({
    peekRestoredPeripheral: jest.fn(() => null),
    consumeRestoredPeripheral: jest.fn(),
}));

jest.mock('@/hooks/use-ble-connection', () => ({
    useBleConnection: jest.fn(),
}));

describe('useBleRestorationAdopt (issue #190)', () => {
    let startReconnectLoop: jest.Mock;

    beforeEach(() => {
        jest.clearAllMocks();
        jest.spyOn(console, 'log').mockImplementation(() => {});
        startReconnectLoop = jest.fn(async () => undefined);
        (BleConnectionHook.useBleConnection as jest.Mock).mockReturnValue({ startReconnectLoop });
    });

    afterEach(() => {
        jest.restoreAllMocks();
    });

    it('no restored peripheral: binds useBleConnection to empty args and never starts the loop', () => {
        renderHook(() => useBleRestorationAdopt());

        expect(BleConnectionHook.useBleConnection).toHaveBeenCalledWith('', '');
        expect(startReconnectLoop).not.toHaveBeenCalled();
        expect(BleHook.consumeRestoredPeripheral).not.toHaveBeenCalled();
    });

    it('restored peripheral: binds to it, consumes the stash, and starts the loop exactly once', () => {
        (BleHook.peekRestoredPeripheral as jest.Mock).mockReturnValue({
            mac: 'PERIPHERAL-UUID-1',
            name: 'RGB Sunglasses A1B2',
        });

        const { rerender } = renderHook(() => useBleRestorationAdopt());

        expect(BleConnectionHook.useBleConnection).toHaveBeenCalledWith('PERIPHERAL-UUID-1', 'RGB Sunglasses A1B2');
        expect(BleHook.consumeRestoredPeripheral).toHaveBeenCalledTimes(1);
        expect(startReconnectLoop).toHaveBeenCalledTimes(1);
        // Consumption happens before the loop starts, so a re-entrant mount
        // during adoption peeks an already-empty stash.
        expect((BleHook.consumeRestoredPeripheral as jest.Mock).mock.invocationCallOrder[0])
            .toBeLessThan(startReconnectLoop.mock.invocationCallOrder[0]);

        // Re-renders don't re-fire the mount effect.
        rerender(undefined);
        expect(startReconnectLoop).toHaveBeenCalledTimes(1);
    });

    it('a remount after consumption does not start the loop again', () => {
        (BleHook.peekRestoredPeripheral as jest.Mock).mockReturnValue({
            mac: 'PERIPHERAL-UUID-1',
            name: 'RGB Sunglasses A1B2',
        });

        const { unmount } = renderHook(() => useBleRestorationAdopt());
        expect(startReconnectLoop).toHaveBeenCalledTimes(1);
        unmount();

        // The real consumeRestoredPeripheral() cleared the stash, so a later
        // mount peeks null. (Modelled by flipping the mock's return, not a
        // mockReturnValueOnce - React may invoke the useState initializer more
        // than once per mount, which would burn extra "once" values.)
        (BleHook.peekRestoredPeripheral as jest.Mock).mockReturnValue(null);
        renderHook(() => useBleRestorationAdopt());
        expect(BleConnectionHook.useBleConnection).toHaveBeenLastCalledWith('', '');
        expect(startReconnectLoop).toHaveBeenCalledTimes(1);
    });
});
