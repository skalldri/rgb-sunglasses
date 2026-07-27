import { fireEvent, render } from '@testing-library/react-native';
import React from 'react';

import { ShuffleButton } from '@/components/shuffle-button';
import { UUID_SHUFFLE_ENABLED } from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import { encodeBooleanToBase64 } from '@/services/ble-value-codec';

function shuffleEnabledInfo(enabled: boolean, extra: object = {}) {
  return {
    characteristic: {},
    value: encodeBooleanToBase64(enabled),
    name: 'Shuffle Enabled',
    cpfFormat: 0x01,
    isUpdateInProgress: false,
    ...extra,
  };
}

function mockBluetooth(characteristics: object, writeToCharacteristic = jest.fn(async () => true)) {
  jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
    selectedDevice: {
      name: 'RGB Sunglasses',
      mac: 'AA:BB:CC',
      device: {},
      services: [],
      characteristicsByService: {},
      characteristics,
      serviceCharacteristics: {},
      serviceDisplayNames: {},
    },
    writeToCharacteristic,
  } as any);
  return writeToCharacteristic;
}

describe('ShuffleButton', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('renders nothing when the Shuffle Enabled characteristic is absent (older firmware)', () => {
    mockBluetooth({});
    const { queryByTestId } = render(<ShuffleButton />);
    expect(queryByTestId('shuffle-button')).toBeNull();
  });

  it('reflects the enabled state and writes the inverse on press', () => {
    const write = mockBluetooth({ [UUID_SHUFFLE_ENABLED]: shuffleEnabledInfo(true) });
    const { getByTestId } = render(<ShuffleButton />);

    const button = getByTestId('shuffle-button');
    expect(button.props.accessibilityState?.checked).toBe(true);

    fireEvent.press(button);
    expect(write).toHaveBeenCalledWith(UUID_SHUFFLE_ENABLED, encodeBooleanToBase64(false));
  });

  it('is disabled while a write is in progress', () => {
    const write = mockBluetooth({
      [UUID_SHUFFLE_ENABLED]: shuffleEnabledInfo(false, { isUpdateInProgress: true }),
    });
    const { getByTestId } = render(<ShuffleButton />);

    const button = getByTestId('shuffle-button');
    expect(button.props.accessibilityState?.disabled).toBe(true);
    fireEvent.press(button);
    expect(write).not.toHaveBeenCalled();
  });

  it('surfaces a failed write through the write-error indicator', () => {
    mockBluetooth({
      [UUID_SHUFFLE_ENABLED]: shuffleEnabledInfo(false, {
        lastWriteError: 'The device refused the change.',
      }),
    });
    const { getByTestId } = render(<ShuffleButton />);
    expect(getByTestId('write-error-indicator')).toBeTruthy();
  });
});
