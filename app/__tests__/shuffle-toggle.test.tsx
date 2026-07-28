import { fireEvent, render } from '@testing-library/react-native';
import React from 'react';

import { ShuffleToggle } from '@/components/shuffle-toggle';
import { UUID_SHUFFLE_INCLUDE_CHARACTERISTIC } from '@/constants/bluetooth';
import { encodeBooleanToBase64 } from '@/services/ble-value-codec';

function makeCharInfo(overrides: object = {}) {
  return {
    characteristic: {},
    value: encodeBooleanToBase64(true),
    name: 'Include in Shuffle',
    cpfFormat: 0x01,
    isUpdateInProgress: false,
    ...overrides,
  } as any;
}

describe('ShuffleToggle', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('reports included state via accessibilityState and writes the inverse on press', () => {
    const onWrite = jest.fn();
    const charInfo = makeCharInfo();
    const { getByTestId } = render(
      <ShuffleToggle
        charUuid={UUID_SHUFFLE_INCLUDE_CHARACTERISTIC}
        charInfo={charInfo}
        onWrite={onWrite}
      />
    );

    const toggle = getByTestId('shuffle-toggle');
    expect(toggle.props.accessibilityState?.checked).toBe(true);

    fireEvent.press(toggle);
    expect(onWrite).toHaveBeenCalledWith(
      UUID_SHUFFLE_INCLUDE_CHARACTERISTIC,
      encodeBooleanToBase64(false),
      charInfo.value
    );
  });

  it('treats an excluded value as unchecked and writes true on press', () => {
    const onWrite = jest.fn();
    const charInfo = makeCharInfo({ value: encodeBooleanToBase64(false) });
    const { getByTestId } = render(
      <ShuffleToggle
        charUuid={UUID_SHUFFLE_INCLUDE_CHARACTERISTIC}
        charInfo={charInfo}
        onWrite={onWrite}
      />
    );

    const toggle = getByTestId('shuffle-toggle');
    expect(toggle.props.accessibilityState?.checked).toBe(false);

    fireEvent.press(toggle);
    expect(onWrite).toHaveBeenCalledWith(
      UUID_SHUFFLE_INCLUDE_CHARACTERISTIC,
      encodeBooleanToBase64(true),
      charInfo.value
    );
  });

  it('is disabled and inert while a write is in progress', () => {
    const onWrite = jest.fn();
    const { getByTestId } = render(
      <ShuffleToggle
        charUuid={UUID_SHUFFLE_INCLUDE_CHARACTERISTIC}
        charInfo={makeCharInfo({ isUpdateInProgress: true })}
        onWrite={onWrite}
      />
    );

    const toggle = getByTestId('shuffle-toggle');
    expect(toggle.props.accessibilityState?.disabled).toBe(true);
    fireEvent.press(toggle);
    expect(onWrite).not.toHaveBeenCalled();
  });

  it('tolerates an undecodable value (renders unchecked, still writable)', () => {
    const onWrite = jest.fn();
    const { getByTestId } = render(
      <ShuffleToggle
        charUuid={UUID_SHUFFLE_INCLUDE_CHARACTERISTIC}
        charInfo={makeCharInfo({ value: '' })}
        onWrite={onWrite}
      />
    );

    const toggle = getByTestId('shuffle-toggle');
    expect(toggle.props.accessibilityState?.checked).toBe(false);
    fireEvent.press(toggle);
    expect(onWrite).toHaveBeenCalledWith(
      UUID_SHUFFLE_INCLUDE_CHARACTERISTIC,
      encodeBooleanToBase64(true),
      ''
    );
  });
});
