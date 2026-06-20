import React from 'react';
import { fireEvent, render, waitFor } from '@testing-library/react-native';
import { Animated } from 'react-native';

import DeviceStateScreen from '@/app/(tabs)/device-state';
import {
  BLE_GATT_CPF_FORMAT_BOOLEAN,
  BLE_GATT_CPF_FORMAT_CUSTOM_COLOR,
  BLE_GATT_CPF_FORMAT_DROPDOWN_LIST,
  BLE_GATT_CPF_FORMAT_UINT32,
  BLE_GATT_CPF_FORMAT_UTF8S,
} from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import {
  encodeBooleanToBase64,
  encodeColorToBase64,
  encodeUint32ToBase64,
  encodeUtf8ToBase64,
} from '@/services/ble-value-codec';

jest.mock('@react-navigation/bottom-tabs', () => ({
  useBottomTabBarHeight: () => 0,
}));

function buildSelectedDevice(characters: Array<{ uuid: string; cpfFormat: number; value: string; name?: string }>) {
  const serviceUuid = 'service-1';
  const byService: Record<string, any> = { [serviceUuid]: {} };
  const flat: Record<string, any> = {};

  for (const char of characters) {
    const info = {
      characteristic: {},
      value: char.value,
      name: char.name ?? char.uuid,
      cpfFormat: char.cpfFormat,
      isUpdateInProgress: false,
    };
    byService[serviceUuid][char.uuid] = info;
    flat[char.uuid] = info;
  }

  return {
    name: 'RGB Sunglasses',
    mac: 'AA:BB:CC',
    device: {},
    services: [{ uuid: serviceUuid }],
    characteristicsByService: byService,
    characteristics: flat,
    serviceCharacteristics: { [serviceUuid]: characters.map(c => c.uuid) },
  };
}

describe('DeviceStateScreen', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
    jest.spyOn(Animated, 'timing').mockImplementation(
      () =>
        ({
          start: (callback?: (result: { finished: boolean }) => void) => callback?.({ finished: true }),
        }) as any
    );
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('shows NOT CONNECTED when no device is selected', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: null,
      writeToCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText } = render(<DeviceStateScreen />);
    expect(getByText('NOT CONNECTED')).toBeTruthy();
  });

  it('encodes boolean switch writes using BLE boolean format', async () => {
    const writeToCharacteristic = jest.fn(async () => true);
    const selectedDevice = buildSelectedDevice([
      {
        uuid: 'bool-char',
        cpfFormat: BLE_GATT_CPF_FORMAT_BOOLEAN,
        value: encodeBooleanToBase64(false),
      },
    ]);

    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice,
      writeToCharacteristic,
    } as any);

    const { getByRole } = render(<DeviceStateScreen />);
    fireEvent(getByRole('switch'), 'valueChange', true);

    await waitFor(() => {
      expect(writeToCharacteristic).toHaveBeenCalledWith('bool-char', encodeBooleanToBase64(true));
    });
  });

  it('encodes UTF-8 text writes on submit', async () => {
    const writeToCharacteristic = jest.fn(async () => true);
    const selectedDevice = buildSelectedDevice([
      {
        uuid: 'utf8-char',
        cpfFormat: BLE_GATT_CPF_FORMAT_UTF8S,
        value: encodeUtf8ToBase64('old-name'),
      },
    ]);

    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice,
      writeToCharacteristic,
    } as any);

    const { getByPlaceholderText } = render(<DeviceStateScreen />);
    const input = getByPlaceholderText('Enter value');

    fireEvent.changeText(input, 'new-name');
    fireEvent(input, 'submitEditing');

    await waitFor(() => {
      expect(writeToCharacteristic).toHaveBeenCalledWith('utf8-char', encodeUtf8ToBase64('new-name'));
    });
  });

  it('sanitizes numeric input and encodes uint32 writes on submit', async () => {
    const writeToCharacteristic = jest.fn(async () => true);
    const selectedDevice = buildSelectedDevice([
      {
        uuid: 'uint-char',
        cpfFormat: BLE_GATT_CPF_FORMAT_UINT32,
        value: encodeUint32ToBase64(7),
      },
    ]);

    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice,
      writeToCharacteristic,
    } as any);

    const { getByPlaceholderText } = render(<DeviceStateScreen />);
    const input = getByPlaceholderText('Enter number');

    fireEvent.changeText(input, '12ab3');
    fireEvent(input, 'submitEditing');

    await waitFor(() => {
      expect(writeToCharacteristic).toHaveBeenCalledWith('uint-char', encodeUint32ToBase64(123));
    });

    fireEvent.changeText(input, '');
    fireEvent(input, 'submitEditing');

    await waitFor(() => {
      expect(writeToCharacteristic).toHaveBeenCalledTimes(1);
    });
  });

  it('renders a color swatch and Pick Color button for color characteristics', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildSelectedDevice([
        {
          uuid: 'color-char',
          cpfFormat: BLE_GATT_CPF_FORMAT_CUSTOM_COLOR,
          value: encodeColorToBase64({ r: 255, g: 128, b: 0 }),
        },
      ]),
      writeToCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText } = render(<DeviceStateScreen />);
    expect(getByText('Pick Color')).toBeTruthy();
  });

  it('renders an inline dropdown for drop-down list characteristics', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildSelectedDevice([
        {
          uuid: 'dropdown-char',
          cpfFormat: BLE_GATT_CPF_FORMAT_DROPDOWN_LIST,
          value: encodeUtf8ToBase64('Loop One\nPlay All\nStop After One'),
        },
      ]),
      writeToCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText, queryByText } = render(<DeviceStateScreen />);
    expect(getByText('Loop One')).toBeTruthy();
    // Collapsed by default: the other options aren't rendered until tapped open.
    expect(queryByText('Play All')).toBeNull();

    fireEvent.press(getByText('Loop One'));
    expect(getByText('Play All')).toBeTruthy();
    expect(getByText('Stop After One')).toBeTruthy();
  });

  it('syncs pendingValues when a BLE notification updates a characteristic value', async () => {
    const writeToCharacteristic = jest.fn(async () => true);
    const spy = jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildSelectedDevice([
        {
          uuid: 'utf8-char',
          cpfFormat: BLE_GATT_CPF_FORMAT_UTF8S,
          value: encodeUtf8ToBase64('initial'),
        },
      ]),
      writeToCharacteristic,
    } as any);

    const { getByPlaceholderText, rerender } = render(<DeviceStateScreen />);

    await waitFor(() => {
      expect(getByPlaceholderText('Enter value').props.value).toBe('initial');
    });

    spy.mockReturnValue({
      selectedDevice: buildSelectedDevice([
        {
          uuid: 'utf8-char',
          cpfFormat: BLE_GATT_CPF_FORMAT_UTF8S,
          value: encodeUtf8ToBase64('updated-by-notification'),
        },
      ]),
      writeToCharacteristic,
    } as any);

    rerender(<DeviceStateScreen />);

    await waitFor(() => {
      expect(getByPlaceholderText('Enter value').props.value).toBe('updated-by-notification');
    });
  });
});
