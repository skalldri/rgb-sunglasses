import React from 'react';
import { act, fireEvent, render } from '@testing-library/react-native';

import CaptureScreen from '@/app/(tabs)/device-state/capture';
import {
  UUID_CAPTURE_CONTROL,
  UUID_CAPTURE_COUNT,
  UUID_CAPTURE_ELAPSED_S,
  UUID_CAPTURE_LIMIT_S,
  UUID_CAPTURE_REMAINING_S,
  UUID_CAPTURE_SERVICE,
  UUID_CAPTURE_STATE,
} from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import { encodeUint32ToBase64 } from '@/services/ble-value-codec';
import { CAPTURE_STATE_FAILED, CAPTURE_STATE_IDLE, CAPTURE_STATE_RECORDING } from '@/services/capture';

jest.mock('@react-navigation/bottom-tabs', () => ({
  useBottomTabBarHeight: () => 0,
}));

// Override the global expo-router mock so the focus effect actually runs — this screen
// subscribes to Capture State/Elapsed only while focused. Same technique as
// device-state-battery.test.tsx.
jest.mock('expo-router', () => {
  // eslint-disable-next-line @typescript-eslint/no-require-imports -- jest.mock factories cannot reference imports
  const ReactActual = require('react');
  return {
    Link: ({ children }: { children: React.ReactNode }) =>
      ReactActual.createElement(ReactActual.Fragment, null, children),
    useRouter: () => ({
      back: jest.fn(), push: jest.fn(), navigate: jest.fn(), replace: jest.fn(),
      canDismiss: jest.fn(() => false), dismissAll: jest.fn(),
    }),
    useFocusEffect: (cb: () => void | (() => void)) => {
      ReactActual.useEffect(() => cb(), [cb]);
    },
    useLocalSearchParams: jest.fn(() => ({})),
  };
});

const UINT32 = 0x08;

function charInfo(value: string, writable: boolean, notifiable = false) {
  return {
    characteristic: {
      isWritableWithResponse: writable,
      isWritableWithoutResponse: false,
      isNotifiable: notifiable,
      // Seed reads: the scoped-monitor hook reads every target once on focus.
      read: jest.fn().mockResolvedValue({ value }),
      monitor: jest.fn(() => ({ remove: jest.fn() })),
    },
    value,
    name: null,
    cpfFormat: UINT32,
    isUpdateInProgress: false,
  };
}

function buildDevice(fixture: {
  state: number;
  limitS?: number;
  elapsedS?: number;
  remainingS?: number;
  count?: number;
}) {
  const captureChars: Record<string, unknown> = {
    [UUID_CAPTURE_CONTROL]: charInfo(encodeUint32ToBase64(0), true),
    [UUID_CAPTURE_LIMIT_S]: charInfo(encodeUint32ToBase64(fixture.limitS ?? 30), true),
    [UUID_CAPTURE_STATE]: charInfo(encodeUint32ToBase64(fixture.state), false, true),
    [UUID_CAPTURE_ELAPSED_S]: charInfo(encodeUint32ToBase64(fixture.elapsedS ?? 0), false, true),
    [UUID_CAPTURE_REMAINING_S]: charInfo(encodeUint32ToBase64(fixture.remainingS ?? 120), false),
    [UUID_CAPTURE_COUNT]: charInfo(encodeUint32ToBase64(fixture.count ?? 0), false),
  };
  return {
    name: 'RGB Sunglasses',
    mac: 'AA:BB:CC',
    device: {},
    services: [{ uuid: UUID_CAPTURE_SERVICE }],
    characteristics: captureChars,
    characteristicsByService: { [UUID_CAPTURE_SERVICE]: captureChars },
    serviceCharacteristics: {},
  };
}

function mockContext(device: unknown, writeToCharacteristic = jest.fn().mockResolvedValue(true)) {
  jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
    selectedDevice: device,
    writeToCharacteristic,
    updateCharValue: jest.fn(),
    updateServiceCharacteristicValue: jest.fn(),
  } as unknown as ReturnType<typeof BluetoothContext.useBluetooth>);
  return writeToCharacteristic;
}

describe('CaptureScreen', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('starts a capture by writing command 1 to Capture Control', async () => {
    const write = mockContext(buildDevice({ state: CAPTURE_STATE_IDLE }));

    const { getByLabelText } = render(<CaptureScreen />);
    // Awaited: the press flips an in-flight flag and clears it when the write
    // settles, so an unawaited press leaves a state update to land after the test.
    await act(async () => { fireEvent.press(getByLabelText('Start recording')); });

    // skipOptimisticUpdate: the control is a command, and Capture State is what
    // reports whether the device accepted it.
    expect(write).toHaveBeenCalledWith(
      UUID_CAPTURE_CONTROL,
      encodeUint32ToBase64(1),
      { skipOptimisticUpdate: true },
    );
  });

  it('shows Stop (and no Record) while the device reports Recording, and writes command 0', async () => {
    const write = mockContext(buildDevice({
      state: CAPTURE_STATE_RECORDING, elapsedS: 7, limitS: 30, remainingS: 120,
    }));

    const { getByLabelText, queryByLabelText, getByText } = render(<CaptureScreen />);

    expect(getByText('Recording')).toBeTruthy();
    expect(getByText('0:07 of 0:30')).toBeTruthy();
    expect(queryByLabelText('Start recording')).toBeNull();

    await act(async () => { fireEvent.press(getByLabelText('Stop recording')); });
    expect(write).toHaveBeenCalledWith(
      UUID_CAPTURE_CONTROL,
      encodeUint32ToBase64(0),
      { skipOptimisticUpdate: true },
    );
  });

  it('reports the clamped length the device will actually record, not the one picked', () => {
    // 60 s requested, 20 s of room: capture_start() clamps rather than failing, so
    // the UI must say so before the button is pressed.
    mockContext(buildDevice({ state: CAPTURE_STATE_IDLE, limitS: 60, remainingS: 20 }));

    const { getByText } = render(<CaptureScreen />);
    expect(getByText('Records up to 0:20 of audio + IMU, then stops on its own.')).toBeTruthy();
    expect(getByText('Only 0:20 will fit — the device stops there.')).toBeTruthy();
  });

  it('disables Record and explains why when the volume is full', () => {
    mockContext(buildDevice({ state: CAPTURE_STATE_IDLE, remainingS: 0, count: 6 }));

    const { getByLabelText, getByText } = render(<CaptureScreen />);
    expect(getByLabelText('Start recording').props.accessibilityState?.disabled).toBe(true);
    expect(getByText(/No room left on the device/)).toBeTruthy();
  });

  it('writes the picked length to Capture Limit S', async () => {
    const write = mockContext(buildDevice({ state: CAPTURE_STATE_IDLE, limitS: 30 }));

    const { getByText } = render(<CaptureScreen />);
    await act(async () => { fireEvent.press(getByText('1:00')); });

    expect(write).toHaveBeenCalledWith(UUID_CAPTURE_LIMIT_S, encodeUint32ToBase64(60));
  });

  it('surfaces a failed capture without pretending the device is idle', () => {
    mockContext(buildDevice({ state: CAPTURE_STATE_FAILED, count: 2 }));

    const { getByText, getByLabelText } = render(<CaptureScreen />);
    expect(getByText('Failed')).toBeTruthy();
    // Retry is still offered — a failure is not a dead end.
    expect(getByLabelText('Start recording')).toBeTruthy();
  });

  it('shows the collection counters', () => {
    mockContext(buildDevice({ state: CAPTURE_STATE_IDLE, count: 4, remainingS: 95 }));

    const { getByText } = render(<CaptureScreen />);
    expect(getByText('Captures on device')).toBeTruthy();
    expect(getByText('4')).toBeTruthy();
    expect(getByText('1:35')).toBeTruthy();
  });

  it('renders the unavailable state on firmware without the capture service', () => {
    mockContext({
      name: 'RGB Sunglasses', mac: 'AA:BB:CC', device: {}, services: [],
      characteristics: {}, characteristicsByService: {}, serviceCharacteristics: {},
    });

    const { getByText } = render(<CaptureScreen />);
    expect(getByText('Not available')).toBeTruthy();
  });
});
