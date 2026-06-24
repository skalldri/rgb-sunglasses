import React from 'react';
import { fireEvent, render, waitFor } from '@testing-library/react-native';

import DeviceStateMenuScreen from '@/app/(tabs)/device-state/index';
import { UUID_IS_ACTIVE_CHARACTERISTIC } from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import { encodeBooleanToBase64 } from '@/services/ble-value-codec';
import { SMP_SERVICE_UUID } from '@/services/mcumgr';

jest.mock('@react-navigation/bottom-tabs', () => ({
  useBottomTabBarHeight: () => 0,
}));

function isActiveInfo(active: boolean) {
  return {
    characteristic: {},
    value: encodeBooleanToBase64(active),
    name: 'Is Active',
    cpfFormat: 0x01,
    isUpdateInProgress: false,
  };
}

describe('DeviceStateMenuScreen', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('shows NOT CONNECTED when no device is selected', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: null,
    } as any);

    const { getByText } = render(<DeviceStateMenuScreen />);
    expect(getByText('NOT CONNECTED')).toBeTruthy();
  });

  it('builds the Animations section purely from live-discovered serviceDisplayNames, not a hardcoded list', () => {
    const selectedDevice = {
      name: 'RGB Sunglasses',
      mac: 'AA:BB:CC',
      device: {},
      services: [{ uuid: 'anim-service-1' }, { uuid: 'core-config-service' }],
      characteristicsByService: {
        'anim-service-1': { [UUID_IS_ACTIVE_CHARACTERISTIC]: isActiveInfo(true) },
        'core-config-service': {},
      },
      characteristics: {},
      serviceCharacteristics: { 'anim-service-1': [], 'core-config-service': [] },
      serviceDisplayNames: { 'anim-service-1': 'Totally Made Up Animation Name' },
    };

    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice,
      writeServiceCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText } = render(<DeviceStateMenuScreen />);
    expect(getByText('Animations')).toBeTruthy();
    expect(getByText('Totally Made Up Animation Name')).toBeTruthy();
    expect(getByText('Settings')).toBeTruthy();
  });

  it('renders an Is Active switch for an animation and toggles it via writeServiceCharacteristic', async () => {
    const writeServiceCharacteristic = jest.fn(async () => true);
    const selectedDevice = {
      name: 'RGB Sunglasses',
      mac: 'AA:BB:CC',
      device: {},
      services: [{ uuid: 'anim-service-1' }],
      characteristicsByService: {
        'anim-service-1': { [UUID_IS_ACTIVE_CHARACTERISTIC]: isActiveInfo(false) },
      },
      characteristics: {},
      serviceCharacteristics: { 'anim-service-1': [] },
      serviceDisplayNames: { 'anim-service-1': 'Rainbow' },
    };

    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice,
      writeServiceCharacteristic,
    } as any);

    const { getByRole } = render(<DeviceStateMenuScreen />);
    fireEvent(getByRole('switch'), 'valueChange', true);

    await waitFor(() => {
      expect(writeServiceCharacteristic).toHaveBeenCalledWith(
        'anim-service-1',
        UUID_IS_ACTIVE_CHARACTERISTIC,
        encodeBooleanToBase64(true)
      );
    });
  });

  it('shows a Firmware Update row for the McuMgr service', () => {
    const selectedDevice = {
      name: 'RGB Sunglasses',
      mac: 'AA:BB:CC',
      device: {},
      services: [{ uuid: SMP_SERVICE_UUID }],
      characteristicsByService: { [SMP_SERVICE_UUID]: {} },
      characteristics: {},
      serviceCharacteristics: { [SMP_SERVICE_UUID]: [] },
      serviceDisplayNames: {},
    };

    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice,
      writeServiceCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText } = render(<DeviceStateMenuScreen />);
    expect(getByText('Firmware Update')).toBeTruthy();
  });
});
