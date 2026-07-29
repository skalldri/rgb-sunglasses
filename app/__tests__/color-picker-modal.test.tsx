import { fireEvent, render, waitFor } from '@testing-library/react-native';
import React from 'react';

import ColorPickerModal from '@/app/color-picker-modal';
import {
  COLOR_MODE_RANDOM_ON_ACTIVATE,
  COLOR_MODE_SPECTRUM_SWEEP,
} from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import { encodeColorToBase64, encodeColorValueToBase64 } from '@/services/ble-value-codec';
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

  it('shows the speed slider (not the wheel) for a sweep-mode value and writes mode+speed', async () => {
    const writeToCharacteristic = jest.fn(async () => true);
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      writeToCharacteristic,
    } as any);
    jest.spyOn(ExpoRouter, 'useLocalSearchParams').mockReturnValue({
      mode: String(COLOR_MODE_SPECTRUM_SWEEP),
      speed: '200',
      r: '0',
      g: '0',
      b: '200',
      charUuid: 'color-char',
    } as any);

    const { getByTestId, queryByTestId, getByText } = render(<ColorPickerModal />);
    expect(getByTestId('speed-slider').props.value).toBe(200);
    expect(queryByTestId('saturation-slider')).toBeNull();
    expect(getByText('Smoothly cycles through the color spectrum.')).toBeTruthy();

    fireEvent.press(getByText('Done'));
    await waitFor(() => {
      expect(writeToCharacteristic).toHaveBeenCalledWith(
        'color-char',
        encodeColorValueToBase64({
          mode: COLOR_MODE_SPECTRUM_SWEEP,
          rgb: { r: 255, g: 0, b: 0 },
          speed: 200,
        })
      );
    });
  });

  it('changes speed via the slider before writing', async () => {
    const writeToCharacteristic = jest.fn(async () => true);
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      writeToCharacteristic,
    } as any);
    jest.spyOn(ExpoRouter, 'useLocalSearchParams').mockReturnValue({
      mode: String(COLOR_MODE_SPECTRUM_SWEEP),
      speed: '128',
      charUuid: 'color-char',
    } as any);

    const { getByTestId, getByText } = render(<ColorPickerModal />);
    fireEvent(getByTestId('speed-slider'), 'valueChange', 55);
    fireEvent.press(getByText('Done'));

    await waitFor(() => {
      const encoded = (writeToCharacteristic.mock.calls[0] as any[])[1] as string;
      expect(atob(encoded).charCodeAt(2)).toBe(55); // r byte = speed
      expect(atob(encoded).charCodeAt(3)).toBe(COLOR_MODE_SPECTRUM_SWEEP);
    });
  });

  it('selects Random on Activate via its pill and writes (0,0,0,3) with no sliders', async () => {
    const writeToCharacteristic = jest.fn(async () => true);
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      writeToCharacteristic,
    } as any);
    jest.spyOn(ExpoRouter, 'useLocalSearchParams').mockReturnValue({
      r: '255',
      g: '0',
      b: '0',
      charUuid: 'color-char',
    } as any);

    const { getByText, queryByTestId } = render(<ColorPickerModal />);
    fireEvent.press(getByText('Random on Activate'));
    expect(queryByTestId('speed-slider')).toBeNull();
    expect(queryByTestId('saturation-slider')).toBeNull();

    fireEvent.press(getByText('Done'));
    await waitFor(() => {
      expect(writeToCharacteristic).toHaveBeenCalledWith(
        'color-char',
        encodeColorValueToBase64({
          mode: COLOR_MODE_RANDOM_ON_ACTIVATE,
          rgb: { r: 255, g: 0, b: 0 },
          speed: 128,
        })
      );
    });
  });

  it('falls back to the static picker for a garbage mode param', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      writeToCharacteristic: jest.fn(async () => true),
    } as any);
    jest.spyOn(ExpoRouter, 'useLocalSearchParams').mockReturnValue({
      mode: '7',
      r: '255',
      g: '0',
      b: '0',
      charUuid: 'color-char',
    } as any);

    const { getByText, getByTestId } = render(<ColorPickerModal />);
    expect(getByText('#FF0000')).toBeTruthy();
    expect(getByTestId('saturation-slider')).toBeTruthy();
  });
});
