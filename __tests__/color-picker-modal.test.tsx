import { fireEvent, render, waitFor } from '@testing-library/react-native';
import React from 'react';

import ColorPickerModal from '@/app/color-picker-modal';
import * as BluetoothContext from '@/context/bluetooth-context';
import { encodeColorToBase64 } from '@/services/ble-value-codec';
import * as ExpoRouter from 'expo-router';

describe('ColorPickerModal', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('calls writeToCharacteristic with the correct encoded color on Done', async () => {
    const writeToCharacteristic = jest.fn(async () => true);
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      writeToCharacteristic,
    } as any);
    jest.spyOn(ExpoRouter, 'useLocalSearchParams').mockReturnValue({
      r: '255',
      g: '128',
      b: '0',
      charUuid: 'color-char',
    } as any);

    const { getByText } = render(<ColorPickerModal />);
    fireEvent.press(getByText('Done'));

    await waitFor(() => {
      expect(writeToCharacteristic).toHaveBeenCalledWith(
        'color-char',
        encodeColorToBase64({ r: 255, g: 128, b: 0 })
      );
    });
  });

  it('does not call writeToCharacteristic when charUuid is absent', async () => {
    const writeToCharacteristic = jest.fn(async () => true);
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      writeToCharacteristic,
    } as any);
    jest.spyOn(ExpoRouter, 'useLocalSearchParams').mockReturnValue({
      r: '255',
      g: '0',
      b: '0',
    } as any);

    const { getByText } = render(<ColorPickerModal />);
    fireEvent.press(getByText('Done'));

    await waitFor(() => {
      expect(writeToCharacteristic).not.toHaveBeenCalled();
    });
  });

  it('renders the color hex label from initial params', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      writeToCharacteristic: jest.fn(async () => true),
    } as any);
    jest.spyOn(ExpoRouter, 'useLocalSearchParams').mockReturnValue({
      r: '255',
      g: '0',
      b: '0',
      charUuid: 'color-char',
    } as any);

    const { getByText } = render(<ColorPickerModal />);
    expect(getByText('#FF0000')).toBeTruthy();
  });
});
