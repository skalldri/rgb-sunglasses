import React from 'react';
import { render } from '@testing-library/react-native';

import { CaptureCard } from '@/components/capture-card';
import { UUID_CAPTURE_COUNT, UUID_CAPTURE_STATE } from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import { encodeUint32ToBase64 } from '@/services/ble-value-codec';
import { CAPTURE_STATE_IDLE, CAPTURE_STATE_RECORDING } from '@/services/capture';

jest.mock('expo-router', () => {
  // eslint-disable-next-line @typescript-eslint/no-require-imports -- jest.mock factories cannot reference imports
  const ReactActual = require('react');
  return {
    Link: ({ children }: { children: React.ReactNode }) =>
      ReactActual.createElement(ReactActual.Fragment, null, children),
    useRouter: jest.fn(),
    useFocusEffect: jest.fn(),
    useLocalSearchParams: jest.fn(() => ({})),
  };
});

function charInfo(value: string) {
  return {
    characteristic: { isWritableWithResponse: false, isWritableWithoutResponse: false },
    value,
    name: null,
    cpfFormat: 0x08,
    isUpdateInProgress: false,
  };
}

function mockDevice(characteristics: Record<string, unknown>) {
  jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
    selectedDevice: {
      name: 'RGB Sunglasses',
      mac: 'AA:BB:CC',
      device: {},
      services: [],
      characteristics,
      characteristicsByService: {},
      serviceCharacteristics: {},
    },
  } as unknown as ReturnType<typeof BluetoothContext.useBluetooth>);
}

describe('CaptureCard (slim tile)', () => {
  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('shows the state badge and how many captures are waiting to be collected', () => {
    mockDevice({
      [UUID_CAPTURE_STATE]: charInfo(encodeUint32ToBase64(CAPTURE_STATE_IDLE)),
      [UUID_CAPTURE_COUNT]: charInfo(encodeUint32ToBase64(3)),
    });

    const { getByText } = render(<CaptureCard />);
    expect(getByText('Ready')).toBeTruthy();
    expect(getByText('3 captures on the device')).toBeTruthy();
  });

  it('singularises one capture', () => {
    mockDevice({
      [UUID_CAPTURE_STATE]: charInfo(encodeUint32ToBase64(CAPTURE_STATE_IDLE)),
      [UUID_CAPTURE_COUNT]: charInfo(encodeUint32ToBase64(1)),
    });

    const { getByText } = render(<CaptureCard />);
    expect(getByText('1 capture on the device')).toBeTruthy();
  });

  it('reflects a running capture', () => {
    mockDevice({
      [UUID_CAPTURE_STATE]: charInfo(encodeUint32ToBase64(CAPTURE_STATE_RECORDING)),
      [UUID_CAPTURE_COUNT]: charInfo(encodeUint32ToBase64(0)),
    });

    const { getByText } = render(<CaptureCard />);
    expect(getByText('Recording')).toBeTruthy();
  });

  it('renders nothing on firmware without the capture service', () => {
    // The app ships independently of firmware and is expected to be updated FIRST,
    // so "no capture service" is a normal state, not an error to surface.
    mockDevice({});

    const { toJSON } = render(<CaptureCard />);
    expect(toJSON()).toBeNull();
  });
});
