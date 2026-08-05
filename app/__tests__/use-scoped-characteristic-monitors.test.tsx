import React from 'react';
import { act, render, waitFor } from '@testing-library/react-native';

// The focus callback must actually run (the global expo-router mock is a no-op), and we
// need to drive blur explicitly to assert unsubscription. `focused` flips between renders.
const focusState = { focused: true };
jest.mock('expo-router', () => {
  // eslint-disable-next-line @typescript-eslint/no-require-imports -- jest.mock factories cannot reference imports
  const ReactActual = require('react');
  return {
    useFocusEffect: (cb: () => void | (() => void)) => {
      ReactActual.useEffect(() => {
        if (!focusState.focused) return undefined;
        return cb();
      }, [cb, focusState.focused]);
    },
  };
});

import {
  BLE_GATT_CPF_FORMAT_DROPDOWN_LIST,
  UUID_IS_ACTIVE_CHARACTERISTIC,
} from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import { useScopedCharacteristicMonitors, type ScopedMonitorTarget } from '@/hooks/use-scoped-characteristic-monitors';

const SVC = 'svc-1';
const CHAR = 'char-1';

function makeCharInfo(opts: {
  notifiable?: boolean;
  readValue?: string | null;
  cpfFormat?: number;
  monitor?: jest.Mock;
  read?: jest.Mock;
} = {}) {
  return {
    characteristic: {
      isNotifiable: opts.notifiable ?? true,
      read: opts.read ?? jest.fn(async () => ({ value: opts.readValue ?? btoa('seed') })),
      monitor: opts.monitor ?? jest.fn(() => ({ remove: jest.fn() })),
    },
    value: null,
    name: 'Label',
    cpfFormat: opts.cpfFormat ?? 0x19,
    isUpdateInProgress: false,
  };
}

function mockContext(charInfo: ReturnType<typeof makeCharInfo>, overrides: Record<string, unknown> = {}) {
  const updateCharValue = jest.fn();
  const updateServiceCharacteristicValue = jest.fn();
  jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
    selectedDevice: { characteristicsByService: { [SVC]: { [CHAR]: charInfo } } },
    updateCharValue,
    updateServiceCharacteristicValue,
    ...overrides,
  } as unknown as ReturnType<typeof BluetoothContext.useBluetooth>);
  return { updateCharValue, updateServiceCharacteristicValue };
}

function Harness({ targets }: { targets: ScopedMonitorTarget[] }) {
  useScopedCharacteristicMonitors(targets);
  return null;
}

const TARGETS: ScopedMonitorTarget[] = [{ serviceUuid: SVC, charUuid: CHAR }];

describe('useScopedCharacteristicMonitors', () => {
  beforeEach(() => {
    focusState.focused = true;
    jest.spyOn(console, 'log').mockImplementation(() => {});
    jest.spyOn(console, 'error').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('seeds via read-then-monitor, then routes notifications into context', async () => {
    // The seed read matters: a screen regaining focus (or a plain reconnect) triggers no
    // notification, so subscribe-only would render stale values indefinitely.
    const info = makeCharInfo({ readValue: btoa('seeded') });
    const { updateCharValue } = mockContext(info);

    render(<Harness targets={TARGETS} />);

    await waitFor(() => {
      expect(updateCharValue).toHaveBeenCalledWith(CHAR, btoa('seeded'));
    });
    expect(info.characteristic.monitor).toHaveBeenCalledTimes(1);

    const cb = (info.characteristic.monitor as jest.Mock).mock.calls[0][0];
    act(() => { cb(null, { value: btoa('pushed') }); });
    expect(updateCharValue).toHaveBeenCalledWith(CHAR, btoa('pushed'));
  });

  it('unsubscribes on blur — this is what frees the Android registration slot', async () => {
    const remove = jest.fn();
    const info = makeCharInfo({ monitor: jest.fn(() => ({ remove })) });
    mockContext(info);

    const { rerender } = render(<Harness targets={TARGETS} />);
    await waitFor(() => expect(info.characteristic.monitor).toHaveBeenCalledTimes(1));

    focusState.focused = false;
    rerender(<Harness targets={TARGETS} />);

    expect(remove).toHaveBeenCalledTimes(1);
  });

  it('does not re-subscribe on context updates that hand back fresh objects', async () => {
    // The feedback loop this guards against: an update lands in context, context returns
    // new object identities, a dependency-carrying callback is recreated and the focus
    // effect re-runs. Hardware showed the read-loop form of this at ~11/second.
    const monitor = jest.fn(() => ({ remove: jest.fn() }));
    jest.spyOn(BluetoothContext, 'useBluetooth').mockImplementation(() => ({
      selectedDevice: { characteristicsByService: { [SVC]: { [CHAR]: makeCharInfo({ monitor }) } } },
      updateCharValue: jest.fn(),
      updateServiceCharacteristicValue: jest.fn(),
    } as unknown as ReturnType<typeof BluetoothContext.useBluetooth>));

    // A fresh targets array each render too — callers build these inline.
    const { rerender } = render(<Harness targets={[{ serviceUuid: SVC, charUuid: CHAR }]} />);
    await waitFor(() => expect(monitor).toHaveBeenCalledTimes(1));

    rerender(<Harness targets={[{ serviceUuid: SVC, charUuid: CHAR }]} />);
    rerender(<Harness targets={[{ serviceUuid: SVC, charUuid: CHAR }]} />);

    expect(monitor).toHaveBeenCalledTimes(1);
  });

  it('ignores a superseded pass’s late callback', async () => {
    // rxandroidble tears a notification down fire-and-forget and does not serialize it
    // against a following re-subscribe, so a stale callback can still fire after blur.
    const info = makeCharInfo();
    const { updateCharValue } = mockContext(info);

    const { rerender } = render(<Harness targets={TARGETS} />);
    await waitFor(() => expect(info.characteristic.monitor).toHaveBeenCalledTimes(1));
    const staleCb = (info.characteristic.monitor as jest.Mock).mock.calls[0][0];

    focusState.focused = false;
    rerender(<Harness targets={TARGETS} />);
    updateCharValue.mockClear();

    act(() => { staleCb(null, { value: btoa('late') }); });
    expect(updateCharValue).not.toHaveBeenCalled();
  });

  it('swallows the cancellation error that remove() delivers', async () => {
    const info = makeCharInfo();
    mockContext(info);
    render(<Harness targets={TARGETS} />);
    await waitFor(() => expect(info.characteristic.monitor).toHaveBeenCalledTimes(1));
    const cb = (info.characteristic.monitor as jest.Mock).mock.calls[0][0];

    // remove() surfaces OperationCancelled through the monitor callback; a dropped link
    // surfaces a disconnect error. Neither is a failure worth logging as one.
    act(() => { cb({ message: 'Operation was cancelled' }, null); });
    act(() => { cb({ message: 'Device was disconnected' }, null); });
    expect(console.error).not.toHaveBeenCalled();

    act(() => { cb({ message: 'something genuinely wrong' }, null); });
    expect(console.error).toHaveBeenCalled();
  });

  it('re-reads dropdown-list characteristics instead of trusting the notified preview', async () => {
    // Firmware notifies only the first token of a dropdown list (bt_gatt_notify cannot
    // fragment), so trusting the notified bytes would collapse the picker to one option.
    const read = jest.fn(async () => ({ value: btoa('Option A\nOption B') }));
    const info = makeCharInfo({ cpfFormat: BLE_GATT_CPF_FORMAT_DROPDOWN_LIST, read });
    const { updateCharValue } = mockContext(info);

    render(<Harness targets={TARGETS} />);
    await waitFor(() => expect(info.characteristic.monitor).toHaveBeenCalledTimes(1));
    const cb = (info.characteristic.monitor as jest.Mock).mock.calls[0][0];

    await act(async () => { await cb(null, { ...info.characteristic, value: btoa('Option A') }); });

    await waitFor(() => {
      expect(updateCharValue).toHaveBeenCalledWith(CHAR, btoa('Option A\nOption B'));
    });
    expect(updateCharValue).not.toHaveBeenCalledWith(CHAR, btoa('Option A'));
  });

  it('routes the reused-UUID characteristics service-aware', async () => {
    const info = makeCharInfo();
    const { updateCharValue, updateServiceCharacteristicValue } = mockContext(info);
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      selectedDevice: { characteristicsByService: { [SVC]: { [UUID_IS_ACTIVE_CHARACTERISTIC]: info } } },
      updateCharValue,
      updateServiceCharacteristicValue,
    } as unknown as ReturnType<typeof BluetoothContext.useBluetooth>);

    render(<Harness targets={[{ serviceUuid: SVC, charUuid: UUID_IS_ACTIVE_CHARACTERISTIC }]} />);
    await waitFor(() => expect(info.characteristic.monitor).toHaveBeenCalledTimes(1));
    const cb = (info.characteristic.monitor as jest.Mock).mock.calls[0][0];

    act(() => { cb(null, { value: btoa('\x01') }); });

    expect(updateServiceCharacteristicValue).toHaveBeenCalledWith(SVC, UUID_IS_ACTIVE_CHARACTERISTIC, btoa('\x01'));
    expect(updateCharValue).not.toHaveBeenCalledWith(UUID_IS_ACTIVE_CHARACTERISTIC, btoa('\x01'));
  });

  it('skips non-notifiable characteristics and survives one with no read()', async () => {
    const monitor = jest.fn(() => ({ remove: jest.fn() }));
    const info = makeCharInfo({ notifiable: false, monitor });
    mockContext(info);
    expect(() => render(<Harness targets={TARGETS} />)).not.toThrow();
    expect(monitor).not.toHaveBeenCalled();

    // A torn-down characteristic has no read() at all; read?.() must not throw.
    const bare = makeCharInfo();
    (bare.characteristic as Record<string, unknown>).read = undefined;
    mockContext(bare);
    expect(() => render(<Harness targets={TARGETS} />)).not.toThrow();
  });
});
