import {
    consumeRestoredPeripheral,
    handleRestoredState,
    RestoredPeripheral,
    subscribeRestoredPeripheral,
} from '@/hooks/ble-manager';

// handleRestoredState is driven directly: the jest.setup.ts BleManager mock
// ignores constructor options, so the restore callback never fires through
// `new BleManager()` in tests. The stash is module-scope, so reset it between
// tests via consumeRestoredPeripheral(); subscribers are unsubscribed by each
// test that registers one.

function makeRestoredDevice(overrides: Record<string, unknown> = {}) {
    return { id: 'PERIPHERAL-UUID-1', localName: 'RGB Sunglasses A1B2', name: 'RGB Sunglasses A1B2', ...overrides };
}

describe('handleRestoredState / subscribeRestoredPeripheral (iOS CB state restoration, issue #190)', () => {
    beforeEach(() => {
        consumeRestoredPeripheral();
        jest.spyOn(console, 'log').mockImplementation(() => {});
        jest.spyOn(console, 'warn').mockImplementation(() => {});
    });

    afterEach(() => {
        jest.restoreAllMocks();
    });

    it('null restored state (first construction) delivers nothing', () => {
        handleRestoredState(null);
        const subscriber = jest.fn();
        const unsubscribe = subscribeRestoredPeripheral(subscriber);
        expect(subscriber).not.toHaveBeenCalled();
        unsubscribe();
    });

    it('an empty connectedPeripherals list delivers nothing', () => {
        handleRestoredState({ connectedPeripherals: [] } as any);
        const subscriber = jest.fn();
        const unsubscribe = subscribeRestoredPeripheral(subscriber);
        expect(subscriber).not.toHaveBeenCalled();
        unsubscribe();
    });

    it('a missing connectedPeripherals array is a no-op, not a crash (runs outside any error boundary)', () => {
        expect(() => handleRestoredState({} as any)).not.toThrow();
        const subscriber = jest.fn();
        const unsubscribe = subscribeRestoredPeripheral(subscriber);
        expect(subscriber).not.toHaveBeenCalled();
        unsubscribe();
    });

    it('callback-before-subscribe: the stashed peripheral is delivered immediately on subscribe', () => {
        handleRestoredState({ connectedPeripherals: [makeRestoredDevice()] } as any);
        const subscriber = jest.fn();
        subscribeRestoredPeripheral(subscriber);
        expect(subscriber).toHaveBeenCalledWith({
            mac: 'PERIPHERAL-UUID-1',
            name: 'RGB Sunglasses A1B2',
        });
    });

    it('subscribe-before-callback: the peripheral is delivered when the async restore event lands', () => {
        const subscriber = jest.fn();
        const unsubscribe = subscribeRestoredPeripheral(subscriber);
        expect(subscriber).not.toHaveBeenCalled();

        handleRestoredState({ connectedPeripherals: [makeRestoredDevice()] } as any);
        expect(subscriber).toHaveBeenCalledWith({
            mac: 'PERIPHERAL-UUID-1',
            name: 'RGB Sunglasses A1B2',
        });
        unsubscribe();
    });

    it('delivers exactly once: a later subscriber gets nothing', () => {
        handleRestoredState({ connectedPeripherals: [makeRestoredDevice()] } as any);
        const first = jest.fn();
        subscribeRestoredPeripheral(first);
        expect(first).toHaveBeenCalledTimes(1);

        const second = jest.fn();
        const unsubscribe = subscribeRestoredPeripheral(second);
        expect(second).not.toHaveBeenCalled();
        unsubscribe();
    });

    it('unsubscribe stops delivery; the peripheral is stashed for a later subscriber instead', () => {
        const first = jest.fn();
        const unsubscribeFirst = subscribeRestoredPeripheral(first);
        unsubscribeFirst();

        handleRestoredState({ connectedPeripherals: [makeRestoredDevice()] } as any);
        expect(first).not.toHaveBeenCalled();

        const second = jest.fn();
        subscribeRestoredPeripheral(second);
        expect(second).toHaveBeenCalledTimes(1);
    });

    it("a stale unsubscribe doesn't remove a newer subscriber", () => {
        const first = jest.fn();
        const unsubscribeFirst = subscribeRestoredPeripheral(first);
        unsubscribeFirst();
        const second = jest.fn();
        subscribeRestoredPeripheral(second);
        // Calling the FIRST unsubscribe again must not detach the second subscriber.
        unsubscribeFirst();

        handleRestoredState({ connectedPeripherals: [makeRestoredDevice()] } as any);
        expect(second).toHaveBeenCalledTimes(1);
    });

    it('falls back localName -> name -> "RGB Sunglasses", treating EMPTY strings like null', () => {
        const deliveries: RestoredPeripheral[] = [];
        const unsubscribe = subscribeRestoredPeripheral(p => deliveries.push(p));

        handleRestoredState({ connectedPeripherals: [makeRestoredDevice({ localName: null, name: 'Fallback Name' })] } as any);
        handleRestoredState({ connectedPeripherals: [makeRestoredDevice({ localName: '', name: 'Fallback Name' })] } as any);
        handleRestoredState({ connectedPeripherals: [makeRestoredDevice({ localName: null, name: null })] } as any);
        handleRestoredState({ connectedPeripherals: [makeRestoredDevice({ localName: '', name: '' })] } as any);

        expect(deliveries.map(d => d.name)).toEqual([
            'Fallback Name',
            'Fallback Name',
            'RGB Sunglasses',
            'RGB Sunglasses',
        ]);
        unsubscribe();
    });

    it('adopts only the FIRST peripheral when several were restored (and warns)', () => {
        const subscriber = jest.fn();
        const unsubscribe = subscribeRestoredPeripheral(subscriber);
        handleRestoredState({
            connectedPeripherals: [
                makeRestoredDevice(),
                makeRestoredDevice({ id: 'PERIPHERAL-UUID-2', localName: 'Other' }),
            ],
        } as any);
        expect(subscriber).toHaveBeenCalledWith(expect.objectContaining({ mac: 'PERIPHERAL-UUID-1' }));
        expect(console.warn).toHaveBeenCalled();
        unsubscribe();
    });
});
