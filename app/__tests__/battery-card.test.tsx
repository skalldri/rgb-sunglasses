import React from 'react';
import { fireEvent, render, waitFor } from '@testing-library/react-native';

import { BatteryCard } from '@/components/battery-card';
import {
  UUID_BATTERY_CHARGE_ENABLE,
  UUID_BATTERY_CHARGE_STATUS,
  UUID_BATTERY_CURRENT,
  UUID_BATTERY_VBUS_CURRENT,
  UUID_BATTERY_VBUS_VOLTAGE,
  UUID_BATTERY_VOLTAGE,
} from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import { encodeBooleanToBase64, encodeUint32ToBase64 } from '@/services/ble-value-codec';

// encodeUint32ToBase64(v >>> 0) yields the two's-complement little-endian bytes the
// firmware's int32 characteristics put on the wire — reused here as an sint32 encoder.
function sint32Value(value: number): string {
  return encodeUint32ToBase64(value >>> 0);
}

function uint8Value(value: number): string {
  return btoa(String.fromCharCode(value & 0xff));
}

function charInfo(value: string, cpfFormat: number) {
  return {
    characteristic: { isWritableWithResponse: false, isWritableWithoutResponse: false },
    value,
    name: null,
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
  chargeEnabled: boolean;
}) {
  return {
    name: 'RGB Sunglasses',
    mac: 'AA:BB:CC',
    device: {},
    services: [],
    characteristicsByService: {},
    characteristics: {
      [UUID_BATTERY_VOLTAGE]: charInfo(sint32Value(fixture.vbatMv), 0x10),
      [UUID_BATTERY_CURRENT]: charInfo(sint32Value(fixture.ibatMa), 0x10),
      [UUID_BATTERY_VBUS_VOLTAGE]: charInfo(sint32Value(fixture.vbusMv), 0x10),
      [UUID_BATTERY_VBUS_CURRENT]: charInfo(sint32Value(fixture.ibusMa), 0x10),
      [UUID_BATTERY_CHARGE_STATUS]: charInfo(uint8Value(fixture.chgStat), 0x04),
      [UUID_BATTERY_CHARGE_ENABLE]: {
        characteristic: { isWritableWithResponse: true, isWritableWithoutResponse: false },
        value: encodeBooleanToBase64(fixture.chargeEnabled),
        name: 'Charging Enabled',
        cpfFormat: 0x01,
        isUpdateInProgress: false,
      },
    },
    serviceCharacteristics: {},
  };
}

describe('BatteryCard', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('renders discharge telemetry: percentage, voltage, current, direction and power', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        vbatMv: 7910, // 70% on the 2S curve
        ibatMa: -350,
        vbusMv: 0,
        ibusMa: 0,
        chgStat: 0, // Not Charging
        chargeEnabled: true,
      }),
      writeToCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText, getAllByText } = render(<BatteryCard />);

    expect(getByText('70% • 7.91 V')).toBeTruthy();
    expect(getByText('-350 mA')).toBeTruthy();
    expect(getByText('Discharging')).toBeTruthy();
    expect(getByText('Not Charging')).toBeTruthy();
    // Battery discharge power and system power are both 7.91 V × 0.35 A ≈ 2.77 W here
    expect(getAllByText('2.77 W').length).toBe(2);
    expect(getByText('Charging Enabled')).toBeTruthy();
  });

  it('renders charge telemetry with a Charging badge and USB-derived system power', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        vbatMv: 8000,
        ibatMa: 500,
        vbusMv: 5000,
        ibusMa: 1000,
        chgStat: 3, // Fast Charge (CC)
        chargeEnabled: true,
      }),
      writeToCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText } = render(<BatteryCard />);

    expect(getByText('500 mA')).toBeTruthy();
    expect(getByText('Charging')).toBeTruthy();
    expect(getByText('Fast Charge (CC)')).toBeTruthy();
    // System = 5 V × 1 A − 8 V × 0.5 A = 1.00 W
    expect(getByText('1.00 W')).toBeTruthy();
    // Power into the battery: 8 V × 0.5 A
    expect(getByText('4.00 W')).toBeTruthy();
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
    } as any);

    const { getByRole } = render(<BatteryCard />);
    fireEvent(getByRole('switch'), 'valueChange', false);

    await waitFor(() => {
      expect(writeToCharacteristic).toHaveBeenCalledWith(
        UUID_BATTERY_CHARGE_ENABLE,
        encodeBooleanToBase64(false)
      );
    });
  });

  it('renders nothing without the battery voltage characteristic', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: {
        name: 'RGB Sunglasses',
        mac: 'AA:BB:CC',
        device: {},
        services: [],
        characteristicsByService: {},
        characteristics: {},
        serviceCharacteristics: {},
      },
      writeToCharacteristic: jest.fn(async () => true),
    } as any);

    const { toJSON } = render(<BatteryCard />);
    expect(toJSON()).toBeNull();
  });
});
