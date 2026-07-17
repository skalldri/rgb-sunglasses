import React from 'react';
import { render } from '@testing-library/react-native';

import { BatteryCard } from '@/components/battery-card';
import {
  UUID_BATTERY_CHARGE_STATUS,
  UUID_BATTERY_PERCENT, UUID_POWER_FLAGS,
  UUID_BATTERY_VOLTAGE,
} from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import { encodeUint32ToBase64 } from '@/services/ble-value-codec';

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

function buildDevice(characteristics: Record<string, unknown>) {
  return {
    name: 'RGB Sunglasses',
    mac: 'AA:BB:CC',
    device: {},
    services: [],
    characteristicsByService: {},
    characteristics,
    serviceCharacteristics: {},
  };
}

describe('BatteryCard (slim tile)', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('prefers the firmware Battery Percent characteristic and shows the charge-status badge', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        // Firmware says 68%; the voltage (7910 mV = 70% on the app curve) must NOT win.
        [UUID_BATTERY_PERCENT]: charInfo(uint8Value(68), 0x04),
        [UUID_BATTERY_VOLTAGE]: charInfo(sint32Value(7910), 0x10),
        [UUID_BATTERY_CHARGE_STATUS]: charInfo(uint8Value(3), 0x04), // Fast Charge (CC)
      }),
    } as any);

    const { getByText, queryByText } = render(<BatteryCard />);

    expect(getByText('68%')).toBeTruthy();
    expect(queryByText('70%')).toBeNull();
    expect(getByText('Fast Charge (CC)')).toBeTruthy();
  });

  it('falls back to the app-side voltage curve on firmware without Battery Percent', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        [UUID_BATTERY_VOLTAGE]: charInfo(sint32Value(7910), 0x10), // 70% on the 2S curve
        [UUID_BATTERY_CHARGE_STATUS]: charInfo(uint8Value(0), 0x04), // Not Charging
      }),
    } as any);

    const { getByText } = render(<BatteryCard />);

    expect(getByText('70%')).toBeTruthy();
    expect(getByText('Not Charging')).toBeTruthy();
  });

  it('is a compact tile: no telemetry rows, only percent + status', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        [UUID_BATTERY_PERCENT]: charInfo(uint8Value(50), 0x04),
        [UUID_BATTERY_CHARGE_STATUS]: charInfo(uint8Value(0), 0x04),
      }),
    } as any);

    const { queryByText, getByText } = render(<BatteryCard />);

    expect(getByText('Battery')).toBeTruthy();
    expect(getByText('50%')).toBeTruthy();
    // The old full-card rows moved to the battery detail page.
    expect(queryByText('System Power')).toBeNull();
    expect(queryByText('Battery Power')).toBeNull();
    expect(queryByText('Charging Enabled')).toBeNull();
  });

  it('is tappable and navigates to the battery detail page', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        [UUID_BATTERY_PERCENT]: charInfo(uint8Value(50), 0x04),
      }),
    } as any);

    const { getByLabelText } = render(<BatteryCard />);
    // Wrapped in <Link href="/(tabs)/device-state/battery" asChild><Pressable>.
    expect(getByLabelText('Battery details')).toBeTruthy();
  });

  it('renders nothing without a percent characteristic or a voltage to derive one', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({}),
    } as any);

    const { toJSON } = render(<BatteryCard />);
    expect(toJSON()).toBeNull();
  });

  it('shows a red No Battery badge when the firmware flags the pack absent', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        [UUID_BATTERY_PERCENT]: charInfo(uint8Value(0), 0x04),
        [UUID_BATTERY_VOLTAGE]: charInfo(sint32Value(1200), 0x10),
        [UUID_BATTERY_CHARGE_STATUS]: charInfo(uint8Value(0), 0x04),
        // Power Flags: VBUS present (bit1) but VBAT absent (bit0 clear).
        [UUID_POWER_FLAGS]: charInfo(uint8Value(0x02), 0x04),
      }),
    } as unknown as ReturnType<typeof BluetoothContext.useBluetooth>);

    const { getByText, queryByText } = render(<BatteryCard />);
    expect(getByText('No Battery')).toBeTruthy();
    expect(queryByText('Not Charging')).toBeNull();
  });

  it('shows the orange Error badge on the firmware comm-error sentinel (0xFF)', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        // Telemetry is stale-but-present during an outage.
        [UUID_BATTERY_PERCENT]: charInfo(uint8Value(68), 0x04),
        [UUID_BATTERY_VOLTAGE]: charInfo(sint32Value(7910), 0x10),
        [UUID_BATTERY_CHARGE_STATUS]: charInfo(uint8Value(0xff), 0x04),
      }),
    } as unknown as ReturnType<typeof BluetoothContext.useBluetooth>);

    const { getByText, queryByText } = render(<BatteryCard />);
    expect(getByText('Error')).toBeTruthy();
    // The stale percent still renders — Error is the staleness signal.
    expect(getByText('68%')).toBeTruthy();
    expect(queryByText('Unknown (255)')).toBeNull();
  });

  it('Error outranks No Battery: presence flags are stale during a comm outage', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        [UUID_BATTERY_PERCENT]: charInfo(uint8Value(0), 0x04),
        [UUID_BATTERY_VOLTAGE]: charInfo(sint32Value(1200), 0x10),
        [UUID_BATTERY_CHARGE_STATUS]: charInfo(uint8Value(0xff), 0x04),
        // Stale flags claim VBAT absent — must not produce "No Battery".
        [UUID_POWER_FLAGS]: charInfo(uint8Value(0x02), 0x04),
      }),
    } as unknown as ReturnType<typeof BluetoothContext.useBluetooth>);

    const { getByText, queryByText } = render(<BatteryCard />);
    expect(getByText('Error')).toBeTruthy();
    expect(queryByText('No Battery')).toBeNull();
  });
});
