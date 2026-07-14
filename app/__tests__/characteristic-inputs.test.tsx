import { fireEvent, render } from '@testing-library/react-native';
import React from 'react';
import { Platform } from 'react-native';

import { CharacteristicFloat32 } from '@/components/characteristic-float32';
import { CharacteristicUint32 } from '@/components/characteristic-uint32';
import { CharacteristicUtf8 } from '@/components/characteristic-utf8';
import { CharacteristicInfo } from '@/context/bluetooth-context';
import {
    decodeFloat32FromBase64,
    encodeFloat32ToBase64,
    encodeUint32ToBase64,
    encodeUtf8ToBase64,
    formatFloat32,
} from '@/services/ble-value-codec';

const originalOS = Object.getOwnPropertyDescriptor(Platform, 'OS')!;

function setPlatform(os: 'ios' | 'android') {
    Object.defineProperty(Platform, 'OS', { get: () => os, configurable: true });
}

function makeCharInfo(value: string): CharacteristicInfo {
    return { value, isUpdateInProgress: false } as unknown as CharacteristicInfo;
}

describe('characteristic text inputs — commit semantics', () => {
    const charUuid = 'test-uuid';
    let onWrite: jest.Mock;
    let onChangeText: jest.Mock;

    beforeEach(() => {
        jest.spyOn(console, 'log').mockImplementation(() => {});
        onWrite = jest.fn();
        onChangeText = jest.fn();
    });

    afterEach(() => {
        Object.defineProperty(Platform, 'OS', originalOS);
        jest.restoreAllMocks();
    });

    function renderUint32(value: string, pendingValue: string) {
        return render(
            <CharacteristicUint32
                charUuid={charUuid}
                charInfo={makeCharInfo(value)}
                pendingValue={pendingValue}
                onChangeText={onChangeText}
                onWrite={onWrite}
            />,
        );
    }

    describe('iOS (dismiss commits, no-op edits skipped)', () => {
        // jest-expo pins Platform.OS to "ios" by default; make it explicit anyway.
        beforeEach(() => setPlatform('ios'));

        it('endEditing skips the write when the display value is unchanged', () => {
            const { getByPlaceholderText } = renderUint32(encodeUint32ToBase64(50), '50');
            fireEvent(getByPlaceholderText('Enter number'), 'endEditing');
            expect(onWrite).not.toHaveBeenCalled();
        });

        it('endEditing writes when the value was edited', () => {
            const stored = encodeUint32ToBase64(50);
            const { getByPlaceholderText } = renderUint32(stored, '60');
            fireEvent(getByPlaceholderText('Enter number'), 'endEditing');
            expect(onWrite).toHaveBeenCalledTimes(1);
            expect(onWrite).toHaveBeenCalledWith(charUuid, encodeUint32ToBase64(60), stored);
        });

        it('endEditing with invalid input writes nothing', () => {
            const { getByPlaceholderText } = renderUint32(encodeUint32ToBase64(50), '');
            fireEvent(getByPlaceholderText('Enter number'), 'endEditing');
            expect(onWrite).not.toHaveBeenCalled();
        });

        it('float32: tap-in/tap-out on an unedited value must NOT write, even when the ' +
            '7-sig-digit display string re-encodes to different bytes (1 ULP)', () => {
            const stored = encodeFloat32ToBase64(1 / 3);
            const display = formatFloat32(decodeFloat32FromBase64(stored));
            // Precondition documenting the trap this test guards: the display string
            // round-trips to DIFFERENT bytes, so a byte-level compare would write.
            expect(encodeFloat32ToBase64(parseFloat(display))).not.toBe(stored);

            const { getByPlaceholderText } = render(
                <CharacteristicFloat32
                    charUuid={charUuid}
                    charInfo={makeCharInfo(stored)}
                    pendingValue={display}
                    onChangeText={onChangeText}
                    onWrite={onWrite}
                />,
            );
            fireEvent(getByPlaceholderText('Enter number'), 'endEditing');
            expect(onWrite).not.toHaveBeenCalled();
        });

        it('submit followed by the blur-driven endEditing writes exactly once', () => {
            const stored = encodeUint32ToBase64(50);
            const { getByPlaceholderText } = renderUint32(stored, '60');
            const input = getByPlaceholderText('Enter number');
            fireEvent(input, 'submitEditing');
            fireEvent(input, 'endEditing');
            expect(onWrite).toHaveBeenCalledTimes(1);
        });

        it('utf8: endEditing commits a changed string and skips an unchanged one', () => {
            const stored = encodeUtf8ToBase64('hello');
            const props = {
                charUuid,
                charInfo: makeCharInfo(stored),
                onChangeText,
                onWrite,
            };

            const unchanged = render(<CharacteristicUtf8 {...props} pendingValue="hello" />);
            fireEvent(unchanged.getByPlaceholderText('Enter value'), 'endEditing');
            expect(onWrite).not.toHaveBeenCalled();

            const changed = render(<CharacteristicUtf8 {...props} pendingValue="world" />);
            fireEvent(changed.getByPlaceholderText('Enter value'), 'endEditing');
            expect(onWrite).toHaveBeenCalledTimes(1);
            expect(onWrite).toHaveBeenCalledWith(charUuid, encodeUtf8ToBase64('world'), stored);
        });
    });

    describe('Android (explicit submit only, tap-away cancels)', () => {
        beforeEach(() => setPlatform('android'));

        it('endEditing does not commit', () => {
            const { getByPlaceholderText } = renderUint32(encodeUint32ToBase64(50), '60');
            fireEvent(getByPlaceholderText('Enter number'), 'endEditing');
            expect(onWrite).not.toHaveBeenCalled();
        });

        it('submitEditing writes unconditionally, even a same-value write', () => {
            const stored = encodeUint32ToBase64(50);
            const { getByPlaceholderText } = renderUint32(stored, '50');
            fireEvent(getByPlaceholderText('Enter number'), 'submitEditing');
            expect(onWrite).toHaveBeenCalledTimes(1);
            expect(onWrite).toHaveBeenCalledWith(charUuid, stored, stored);
        });
    });
});
