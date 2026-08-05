import React from 'react';
import { render, waitFor } from '@testing-library/react-native';

// Override the global expo-router mock (useFocusEffect is a no-op jest.fn() there) so the
// focus callback actually runs — that is what drives the Power Flags re-read this card
// relies on since Power Flags stopped notifying (Android notification-budget fix).
// Same technique as use-disconnect-redirect.test.tsx.
jest.mock('expo-router', () => {
  // eslint-disable-next-line @typescript-eslint/no-require-imports -- jest.mock factories cannot reference imports
  const ReactActual = require('react');
  return {
    Link: ({ children }: { children: React.ReactNode }) =>
      ReactActual.createElement(ReactActual.Fragment, null, children),
    useRouter: jest.fn(),
    useFocusEffect: (cb: () => void | (() => void)) => {
      ReactActual.useEffect(() => cb(), [cb]);
    },
    useLocalSearchParams: jest.fn(() => ({})),
  };
});

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

  it('re-reads Power Flags on focus when it is not notifiable, and applies the fresh value', async () => {
    // Power Flags drives the "No Battery" badge but stopped notifying with the
    // notification-budget fix, so the card re-reads it whenever the screen regains
    // focus. Device returns VBAT_PRESENT clear (0x02) → badge must appear.
    const read = jest.fn().mockResolvedValue({ value: uint8Value(0x02) });
    const updateCharValue = jest.fn();
    const flags = charInfo(uint8Value(0x01), 0x04);
    (flags.characteristic as Record<string, unknown>).isNotifiable = false;
    (flags.characteristic as Record<string, unknown>).read = read;

    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        [UUID_BATTERY_PERCENT]: charInfo(uint8Value(50), 0x04),
        [UUID_BATTERY_VOLTAGE]: charInfo(sint32Value(7500), 0x10),
        [UUID_BATTERY_CHARGE_STATUS]: charInfo(uint8Value(0), 0x04),
        [UUID_POWER_FLAGS]: flags,
      }),
      updateCharValue,
    } as unknown as ReturnType<typeof BluetoothContext.useBluetooth>);

    render(<BatteryCard />);

    await waitFor(() => {
      expect(read).toHaveBeenCalled();
      expect(updateCharValue).toHaveBeenCalledWith(UUID_POWER_FLAGS, uint8Value(0x02));
    });
  });

  it('reads Power Flags ONCE per focus even as context updates hand back fresh objects', async () => {
    // Regression (hardware-observed 2026-08-05): depending on [powerFlagsInfo,
    // updateCharValue] made this a feedback loop — the read calls updateCharValue, the
    // context returns a new characteristics object, the useCallback is invalidated, the
    // focus effect re-runs. Measured at ~11 reads/second against the real device, which
    // saturates the GATT queue. Each re-render below simulates one such context update.
    const read = jest.fn().mockResolvedValue({ value: uint8Value(0x02) });
    function freshDevice() {
      const flags = charInfo(uint8Value(0x01), 0x04);
      (flags.characteristic as Record<string, unknown>).isNotifiable = false;
      (flags.characteristic as Record<string, unknown>).read = read;
      return buildDevice({
        [UUID_BATTERY_PERCENT]: charInfo(uint8Value(50), 0x04),
        [UUID_BATTERY_VOLTAGE]: charInfo(sint32Value(7500), 0x10),
        [UUID_POWER_FLAGS]: flags,
      });
    }

    jest.spyOn(BluetoothContext, 'useBluetooth').mockImplementation(() => ({
      selectedDevice: freshDevice(),   // new object identity on every render
      updateCharValue: jest.fn(),
    } as unknown as ReturnType<typeof BluetoothContext.useBluetooth>));

    const { rerender } = render(<BatteryCard />);
    await waitFor(() => expect(read).toHaveBeenCalledTimes(1));

    rerender(<BatteryCard />);
    rerender(<BatteryCard />);
    rerender(<BatteryCard />);

    // Still exactly one read: re-renders must not re-arm the focus effect.
    expect(read).toHaveBeenCalledTimes(1);
  });

  it('does NOT re-read Power Flags on focus while it is still notifiable', async () => {
    const read = jest.fn().mockResolvedValue({ value: uint8Value(0x02) });
    const flags = charInfo(uint8Value(0x01), 0x04);
    (flags.characteristic as Record<string, unknown>).isNotifiable = true;
    (flags.characteristic as Record<string, unknown>).read = read;

    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        [UUID_BATTERY_PERCENT]: charInfo(uint8Value(50), 0x04),
        [UUID_BATTERY_VOLTAGE]: charInfo(sint32Value(7500), 0x10),
        [UUID_POWER_FLAGS]: flags,
      }),
      updateCharValue: jest.fn(),
    } as unknown as ReturnType<typeof BluetoothContext.useBluetooth>);

    render(<BatteryCard />);
    expect(read).not.toHaveBeenCalled();
  });

  it('survives a focus read that rejects (mid-disconnect) without throwing', async () => {
    const read = jest.fn().mockRejectedValue(new Error('disconnected'));
    const flags = charInfo(uint8Value(0x01), 0x04);
    (flags.characteristic as Record<string, unknown>).isNotifiable = false;
    (flags.characteristic as Record<string, unknown>).read = read;

    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: buildDevice({
        [UUID_BATTERY_PERCENT]: charInfo(uint8Value(50), 0x04),
        [UUID_BATTERY_VOLTAGE]: charInfo(sint32Value(7500), 0x10),
        [UUID_POWER_FLAGS]: flags,
      }),
      updateCharValue: jest.fn(),
    } as unknown as ReturnType<typeof BluetoothContext.useBluetooth>);

    expect(() => render(<BatteryCard />)).not.toThrow();
    await waitFor(() => expect(read).toHaveBeenCalled());
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
