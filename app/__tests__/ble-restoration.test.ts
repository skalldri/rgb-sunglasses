import {
    consumeRestoredPeripheral,
    handleRestoredState,
    peekRestoredPeripheral,
} from '@/hooks/ble-manager';

// handleRestoredState is driven directly: the jest.setup.ts BleManager mock
// ignores constructor options, so the restore callback never fires through
// `new BleManager()` in tests. The stash is module-scope, so reset it between
// tests via consumeRestoredPeripheral().

function makeRestoredDevice(overrides: Record<string, unknown> = {}) {
    return { id: 'PERIPHERAL-UUID-1', localName: 'RGB Sunglasses A1B2', name: 'RGB Sunglasses A1B2', ...overrides };
}

describe('handleRestoredState (iOS CB state restoration stash, issue #190)', () => {
    beforeEach(() => {
        consumeRestoredPeripheral();
        jest.spyOn(console, 'log').mockImplementation(() => {});
        jest.spyOn(console, 'warn').mockImplementation(() => {});
    });

    afterEach(() => {
        jest.restoreAllMocks();
    });

    it('null restored state (first construction) leaves the stash empty', () => {
        handleRestoredState(null);
        expect(peekRestoredPeripheral()).toBeNull();
    });

    it('an empty connectedPeripherals list leaves the stash empty', () => {
        handleRestoredState({ connectedPeripherals: [] } as any);
        expect(peekRestoredPeripheral()).toBeNull();
    });

    it('stashes the restored peripheral as { mac: id, name: localName }', () => {
        handleRestoredState({ connectedPeripherals: [makeRestoredDevice()] } as any);
        expect(peekRestoredPeripheral()).toEqual({
            mac: 'PERIPHERAL-UUID-1',
            name: 'RGB Sunglasses A1B2',
        });
    });

    it('peek is a pure read (repeatable); consume clears the stash', () => {
        handleRestoredState({ connectedPeripherals: [makeRestoredDevice()] } as any);
        const first = peekRestoredPeripheral();
        expect(peekRestoredPeripheral()).toEqual(first);
        consumeRestoredPeripheral();
        expect(peekRestoredPeripheral()).toBeNull();
    });

    it('falls back localName -> name -> "RGB Sunglasses" for the display name', () => {
        handleRestoredState({
            connectedPeripherals: [makeRestoredDevice({ localName: null, name: 'Fallback Name' })],
        } as any);
        expect(peekRestoredPeripheral()?.name).toBe('Fallback Name');

        consumeRestoredPeripheral();
        handleRestoredState({
            connectedPeripherals: [makeRestoredDevice({ localName: null, name: null })],
        } as any);
        expect(peekRestoredPeripheral()?.name).toBe('RGB Sunglasses');
    });

    it('adopts only the FIRST peripheral when several were restored (and warns)', () => {
        handleRestoredState({
            connectedPeripherals: [
                makeRestoredDevice(),
                makeRestoredDevice({ id: 'PERIPHERAL-UUID-2', localName: 'Other' }),
            ],
        } as any);
        expect(peekRestoredPeripheral()?.mac).toBe('PERIPHERAL-UUID-1');
        expect(console.warn).toHaveBeenCalled();
    });
});
