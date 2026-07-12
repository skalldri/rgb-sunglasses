import React from 'react';
import { fireEvent, render, waitFor } from '@testing-library/react-native';
import { Animated } from 'react-native';

import BatteryDetailScreen from '@/app/(tabs)/device-state/battery';
import {
  UUID_BATTERY_CHARGE_CURRENT,
  UUID_BATTERY_CHARGE_ENABLE,
  UUID_BATTERY_CHARGE_STATUS,
  UUID_BATTERY_CURRENT,
  UUID_BATTERY_PERCENT,
  UUID_BATTERY_SERVICE,
  UUID_BATTERY_VBUS_CURRENT,
  UUID_BATTERY_VBUS_VOLTAGE,
  UUID_BATTERY_VOLTAGE,
  UUID_POWER_DEBUG_SERVICE,
} from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import { encodeBooleanToBase64, encodeUint32ToBase64 } from '@/services/ble-value-codec';

jest.mock('@react-navigation/bottom-tabs', () => ({
  useBottomTabBarHeight: () => 0,
}));

// encodeUint32ToBase64(v >>> 0) yields the two's-complement little-endian bytes the
// firmware's int32 characteristics put on the wire — reused here as an sint32 encoder.
function sint32Value(value: number): string {
  return encodeUint32ToBase64(value >>> 0);
}

function uint8Value(value: number): string {
  return btoa(String.fromCharCode(value & 0xff));
}

function readonlyInfo(value: string, cpfFormat: number, name?: string) {
  return {
    characteristic: { isWritableWithResponse: false, isWritableWithoutResponse: false },
    value,
    name: name ?? null,
    cpfFormat,
    isUpdateInProgress: false,
  };
}

function buildDevice(fixture: {
  vbatMv: number;
  ibatMa: number;
  vbusMv: number;
  ibusMa: number;
  chgStat: number;
  percent?: number;
  chargeEnabled?: boolean;
  chargeCurrentMa?: number;
  withPowerDebug?: boolean;
}) {
  const batteryChars: Record<string, any> = {
    [UUID_BATTERY_VOLTAGE]: readonlyInfo(sint32Value(fixture.vbatMv), 0x10),
    [UUID_BATTERY_CURRENT]: readonlyInfo(sint32Value(fixture.ibatMa), 0x10),
    [UUID_BATTERY_VBUS_VOLTAGE]: readonlyInfo(sint32Value(fixture.vbusMv), 0x10),
    [UUID_BATTERY_VBUS_CURRENT]: readonlyInfo(sint32Value(fixture.ibusMa), 0x10),
    [UUID_BATTERY_CHARGE_STATUS]: readonlyInfo(uint8Value(fixture.chgStat), 0x04),
  };
  if (fixture.percent != null) {
    batteryChars[UUID_BATTERY_PERCENT] = readonlyInfo(uint8Value(fixture.percent), 0x04, 'Battery Percent');
  }
  if (fixture.chargeEnabled != null) {
    batteryChars[UUID_BATTERY_CHARGE_ENABLE] = {
      characteristic: { isWritableWithResponse: true, isWritableWithoutResponse: false },
      value: encodeBooleanToBase64(fixture.chargeEnabled),
      name: 'Charging Enabled',
      cpfFormat: 0x01,
      isUpdateInProgress: false,
    };
  }
  if (fixture.chargeCurrentMa != null) {
    batteryChars[UUID_BATTERY_CHARGE_CURRENT] = {
      characteristic: { isWritableWithResponse: true, isWritableWithoutResponse: false },
      value: encodeUint32ToBase64(fixture.chargeCurrentMa),
      name: 'Charge Current (mA)',
      cpfFormat: 0x08,
      isUpdateInProgress: false,
    };
  }

  const powerDebugChars: Record<string, any> = fixture.withPowerDebug
    ? {
        [`${UUID_POWER_DEBUG_SERVICE.slice(0, -4)}0000`]: readonlyInfo(
          encodeUint32ToBase64(3000), 0x08, 'Input Limit (mA)'),
        [`${UUID_POWER_DEBUG_SERVICE.slice(0, -4)}0001`]: readonlyInfo(
          uint8Value(0b0000011), 0x04, 'Power Flags'),
      }
    : {};

  const services = [{ uuid: UUID_BATTERY_SERVICE }];
  const characteristicsByService: Record<string, any> = { [UUID_BATTERY_SERVICE]: batteryChars };
  if (fixture.withPowerDebug) {
    services.push({ uuid: UUID_POWER_DEBUG_SERVICE });
    characteristicsByService[UUID_POWER_DEBUG_SERVICE] = powerDebugChars;
  }

  return {
    name: 'RGB Sunglasses',
    mac: 'AA:BB:CC',
    device: {},
    services,
    characteristicsByService,
    characteristics: { ...batteryChars, ...powerDebugChars },
    serviceCharacteristics: {},
  };
}

describe('BatteryDetailScreen', () => {
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

  it('shows an empty state when no device is selected', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: null,
      writeToCharacteristic: jest.fn(async () => true),
      writeServiceCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText } = render(<BatteryDetailScreen />);
    expect(getByText('Not available')).toBeTruthy();
  });

  it('renders the curated telemetry rows with derived percentage and watts', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        vbatMv: 7910, // 70% on the 2S curve (no fw percent → fallback path)
        ibatMa: -350,
        vbusMv: 0,
        ibusMa: 0,
        chgStat: 0, // Not Charging
      }),
      writeToCharacteristic: jest.fn(async () => true),
      writeServiceCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText, getAllByText } = render(<BatteryDetailScreen />);

    expect(getByText('70% • 7.91 V')).toBeTruthy();
    expect(getByText('-350 mA')).toBeTruthy();
    expect(getByText('Discharging')).toBeTruthy();
    expect(getByText('Not Charging')).toBeTruthy();
    // Battery discharge power and system power are both 7.91 V × 0.35 A ≈ 2.77 W here
    expect(getAllByText('2.77 W').length).toBe(2);
  });

  it('prefers the firmware Battery Percent over the voltage-derived estimate', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        vbatMv: 7910, // would be 70% via the app curve
        ibatMa: 500,
        vbusMv: 5000,
        ibusMa: 1000,
        chgStat: 3,
        percent: 68,
      }),
      writeToCharacteristic: jest.fn(async () => true),
      writeServiceCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText, queryByText } = render(<BatteryDetailScreen />);
    expect(getByText('68% • 7.91 V')).toBeTruthy();
    expect(queryByText('70% • 7.91 V')).toBeNull();
  });

  it('writes the charging toggle through writeToCharacteristic', async () => {
    const writeToCharacteristic = jest.fn(async () => true);
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        vbatMv: 7910,
        ibatMa: -350,
        vbusMv: 0,
        ibusMa: 0,
        chgStat: 0,
        chargeEnabled: true,
      }),
      writeToCharacteristic,
      writeServiceCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByRole } = render(<BatteryDetailScreen />);
    fireEvent(getByRole('switch'), 'valueChange', false);

    await waitFor(() => {
      expect(writeToCharacteristic).toHaveBeenCalledWith(
        UUID_BATTERY_CHARGE_ENABLE,
        encodeBooleanToBase64(false)
      );
    });
  });

  it('renders Charge Current as an editable uint32 input and writes on submit', async () => {
    const writeToCharacteristic = jest.fn(async () => true);
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        vbatMv: 7910,
        ibatMa: -350,
        vbusMv: 0,
        ibusMa: 0,
        chgStat: 0,
        chargeCurrentMa: 900,
      }),
      writeToCharacteristic,
      writeServiceCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByPlaceholderText } = render(<BatteryDetailScreen />);
    const input = getByPlaceholderText('Enter number');

    // Initial pending value decodes from the characteristic.
    await waitFor(() => {
      expect(input.props.value).toBe('900');
    });

    fireEvent.changeText(input, '1200');
    fireEvent(input, 'submitEditing');

    await waitFor(() => {
      expect(writeToCharacteristic).toHaveBeenCalledWith(
        UUID_BATTERY_CHARGE_CURRENT,
        encodeUint32ToBase64(1200)
      );
    });
  });

  it('renders a generic Power Debug section when the service is present', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        vbatMv: 7910,
        ibatMa: 500,
        vbusMv: 5000,
        ibusMa: 1000,
        chgStat: 3,
        withPowerDebug: true,
      }),
      writeToCharacteristic: jest.fn(async () => true),
      writeServiceCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText } = render(<BatteryDetailScreen />);
    expect(getByText('Power Debug')).toBeTruthy();
    expect(getByText('Input Limit (mA)')).toBeTruthy();
    expect(getByText('3000')).toBeTruthy(); // read-only uint32 renders as plain text
    expect(getByText('Power Flags')).toBeTruthy();
  });

  it('omits the Power Debug section on firmware without the service', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        vbatMv: 7910,
        ibatMa: -350,
        vbusMv: 0,
        ibusMa: 0,
        chgStat: 0,
      }),
      writeToCharacteristic: jest.fn(async () => true),
      writeServiceCharacteristic: jest.fn(async () => true),
    } as any);

    const { queryByText } = render(<BatteryDetailScreen />);
    expect(queryByText('Power Debug')).toBeNull();
  });
});
