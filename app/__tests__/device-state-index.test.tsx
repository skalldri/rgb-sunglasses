import React from 'react';
import { fireEvent, render, waitFor } from '@testing-library/react-native';

import DeviceStateMenuScreen from '@/app/(tabs)/device-state/index';
import { UUID_IS_ACTIVE_CHARACTERISTIC, UUID_MCUBOOT_INFO_SERVICE, UUID_MCUBOOT_UPDATER_SERVICE } from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import { encodeBooleanToBase64, encodeUtf8ToBase64 } from '@/services/ble-value-codec';
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

  it('shows the write-error indicator only on the animation whose Is Active write failed (issue #92, care point 2)', () => {
    const failedInfo = { ...isActiveInfo(false), lastWriteError: 'The device refused the change.' };
    const selectedDevice = {
      name: 'RGB Sunglasses',
      mac: 'AA:BB:CC',
      device: {},
      services: [{ uuid: 'anim-service-1' }, { uuid: 'anim-service-2' }],
      characteristicsByService: {
        // Same reused Is Active UUID across both animations; only service-1's write failed.
        'anim-service-1': { [UUID_IS_ACTIVE_CHARACTERISTIC]: failedInfo },
        'anim-service-2': { [UUID_IS_ACTIVE_CHARACTERISTIC]: isActiveInfo(true) },
      },
      characteristics: {},
      serviceCharacteristics: { 'anim-service-1': [], 'anim-service-2': [] },
      serviceDisplayNames: { 'anim-service-1': 'Rainbow', 'anim-service-2': 'Plasma' },
    };

    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice,
      writeServiceCharacteristic: jest.fn(async () => true),
    } as any);

    const { getAllByTestId } = render(<DeviceStateMenuScreen />);
    // Exactly one row lights up, not both (they share the ...-bbbb-...0000 Is Active UUID).
    expect(getAllByTestId('write-error-indicator')).toHaveLength(1);
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

  it('excludes the MCUboot Info and MCUboot Updater services from the Settings section (issue #76)', () => {
    // The "Device Info"/"MCUboot Version" card and the raw-UUID "MCUboot Updater" detail page
    // both moved to the Firmware Update modal (issue #76) - this screen should show neither.
    const versionCharUuid = '12345678-1234-5678-0003-56789abc0001';
    const selectedDevice = {
      name: 'RGB Sunglasses',
      mac: 'AA:BB:CC',
      device: {},
      services: [
        { uuid: UUID_MCUBOOT_INFO_SERVICE },
        { uuid: UUID_MCUBOOT_UPDATER_SERVICE },
        { uuid: 'core-config-service' },
      ],
      characteristicsByService: {
        [UUID_MCUBOOT_INFO_SERVICE]: {
          [versionCharUuid]: {
            characteristic: {},
            value: encodeUtf8ToBase64('1.0.0+0'),
            name: 'MCUboot Version',
            cpfFormat: 0x19,
            isUpdateInProgress: false,
          },
        },
        [UUID_MCUBOOT_UPDATER_SERVICE]: {},
        'core-config-service': {},
      },
      characteristics: {},
      serviceCharacteristics: {
        [UUID_MCUBOOT_INFO_SERVICE]: [],
        [UUID_MCUBOOT_UPDATER_SERVICE]: [],
        'core-config-service': [],
      },
      serviceDisplayNames: {},
    };

    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice,
      writeServiceCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText, queryByText } = render(<DeviceStateMenuScreen />);
    expect(queryByText('Device Info')).toBeNull();
    expect(queryByText('MCUboot Version')).toBeNull();
    expect(queryByText('MCUboot Updater')).toBeNull();
    expect(getByText('Settings')).toBeTruthy();
    // core-config-service is still in Settings (unknown UUID renders as the UUID string itself)
    expect(getByText('core-config-service')).toBeTruthy();
  });
});
