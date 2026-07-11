import React from 'react';
import { fireEvent, render } from '@testing-library/react-native';
import { Animated } from 'react-native';

import DeviceStateDetailScreen from '@/app/(tabs)/device-state/[serviceUuid]';
import { BLE_GATT_CPF_FORMAT_UINT32 } from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import { encodeUint32ToBase64 } from '@/services/ble-value-codec';
import * as ExpoRouter from 'expo-router';

jest.mock('@react-navigation/bottom-tabs', () => ({
  useBottomTabBarHeight: () => 0,
}));

const SERVICE_UUID = 'service-1';

function buildSelectedDevice(characters: { uuid: string; cpfFormat: number; value: string; name?: string }[]) {
  const byService: Record<string, any> = { [SERVICE_UUID]: {} };
  const flat: Record<string, any> = {};

  for (const char of characters) {
    const info = {
      characteristic: {},
      value: char.value,
      name: char.name ?? char.uuid,
      cpfFormat: char.cpfFormat,
      isUpdateInProgress: false,
    };
    byService[SERVICE_UUID][char.uuid] = info;
    flat[char.uuid] = info;
  }

  return {
    name: 'RGB Sunglasses',
    mac: 'AA:BB:CC',
    device: {},
    services: [{ uuid: SERVICE_UUID }],
    characteristicsByService: byService,
    characteristics: flat,
    serviceCharacteristics: { [SERVICE_UUID]: characters.map(c => c.uuid) },
  };
}

function mockBluetooth(selectedDevice: any) {
  return jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
    selectedDevice,
    writeToCharacteristic: jest.fn(async () => true),
    writeServiceCharacteristic: jest.fn(async () => true),
  } as any);
}

describe('useCharacteristicEditor notification sync', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
    jest.spyOn(ExpoRouter, 'useLocalSearchParams').mockReturnValue({ serviceUuid: SERVICE_UUID } as any);
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

  it('keeps an in-progress edit when an unrelated characteristic notifies', () => {
    const spy = mockBluetooth(
      buildSelectedDevice([
        { uuid: 'speed-char', cpfFormat: BLE_GATT_CPF_FORMAT_UINT32, value: encodeUint32ToBase64(50) },
        { uuid: 'vbus-char', cpfFormat: BLE_GATT_CPF_FORMAT_UINT32, value: encodeUint32ToBase64(1000) },
      ])
    );

    const { getByDisplayValue, rerender } = render(<DeviceStateDetailScreen />);

    // User starts editing speed (no submit yet)
    fireEvent.changeText(getByDisplayValue('50'), '217');
    expect(getByDisplayValue('217')).toBeTruthy();

    // An unrelated telemetry notification lands: vbus changes, speed's stored value does not.
    // The context rebuilds the characteristics map with fresh object references (as
    // updateCharValue does), which re-runs the sync effect.
    spy.mockReturnValue({
      selectedDevice: buildSelectedDevice([
        { uuid: 'speed-char', cpfFormat: BLE_GATT_CPF_FORMAT_UINT32, value: encodeUint32ToBase64(50) },
        { uuid: 'vbus-char', cpfFormat: BLE_GATT_CPF_FORMAT_UINT32, value: encodeUint32ToBase64(2000) },
      ]),
      writeToCharacteristic: jest.fn(async () => true),
      writeServiceCharacteristic: jest.fn(async () => true),
    } as any);
    rerender(<DeviceStateDetailScreen />);

    // The in-progress edit must survive; the notified characteristic must sync.
    expect(getByDisplayValue('217')).toBeTruthy();
    expect(getByDisplayValue('2000')).toBeTruthy();
  });

  it('still syncs a characteristic whose own stored value changes', () => {
    const spy = mockBluetooth(
      buildSelectedDevice([
        { uuid: 'speed-char', cpfFormat: BLE_GATT_CPF_FORMAT_UINT32, value: encodeUint32ToBase64(50) },
      ])
    );

    const { getByDisplayValue, rerender } = render(<DeviceStateDetailScreen />);
    expect(getByDisplayValue('50')).toBeTruthy();

    // Device-originated change to the same characteristic (user not editing)
    spy.mockReturnValue({
      selectedDevice: buildSelectedDevice([
        { uuid: 'speed-char', cpfFormat: BLE_GATT_CPF_FORMAT_UINT32, value: encodeUint32ToBase64(99) },
      ]),
      writeToCharacteristic: jest.fn(async () => true),
      writeServiceCharacteristic: jest.fn(async () => true),
    } as any);
    rerender(<DeviceStateDetailScreen />);

    expect(getByDisplayValue('99')).toBeTruthy();
  });
});
