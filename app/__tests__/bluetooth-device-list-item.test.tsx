import React from 'react';
import { fireEvent, render, waitFor } from '@testing-library/react-native';

import BluetoothDeviceListItem from '@/components/bluetooth-device-list-item';
import * as BluetoothContext from '@/context/bluetooth-context';
import * as BleConnectionHook from '@/hooks/use-ble-connection';

jest.mock('@/context/bluetooth-context', () => {
  const actual = jest.requireActual('@/context/bluetooth-context');
  return { ...actual, useBluetooth: jest.fn() };
});

jest.mock('@/hooks/use-ble-connection', () => ({
  useBleConnection: jest.fn(),
}));

// Override the global expo-router mock so useRouter returns a controllable instance
jest.mock('expo-router', () => ({
  Link: ({ children }: { children: unknown }) => {
    const React = require('react');
    return React.createElement(React.Fragment, null, children);
  },
  useRouter: jest.fn(),
  useFocusEffect: jest.fn(),
  useLocalSearchParams: jest.fn(() => ({})),
}));

import * as ExpoRouter from 'expo-router';

let mockRouter: { navigate: jest.Mock; push: jest.Mock; replace: jest.Mock; back: jest.Mock };

describe('BluetoothDeviceListItem', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
    mockRouter = { navigate: jest.fn(), push: jest.fn(), replace: jest.fn(), back: jest.fn() };
    (ExpoRouter.useRouter as jest.Mock).mockReturnValue(mockRouter);
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  function setupMocks({
    selectedMac = null as string | null,
    isConnecting = false,
    connect = jest.fn(async () => {}),
    disconnect = jest.fn(async () => {}),
  } = {}) {
    (BluetoothContext.useBluetooth as jest.Mock).mockReturnValue({
      selectedDevice: selectedMac ? { mac: selectedMac } : null,
    });
    (BleConnectionHook.useBleConnection as jest.Mock).mockReturnValue({
      isConnecting,
      connect,
      disconnect,
    });
    return { connect, disconnect };
  }

  it('shows "Connect" when device is not selected', () => {
    setupMocks({ selectedMac: null });
    const { getByText } = render(<BluetoothDeviceListItem deviceName="RGB" macAddress="AA:BB:CC" />);
    expect(getByText('Connect')).toBeTruthy();
  });

  it('shows "Disconnect" when this device is selected', () => {
    setupMocks({ selectedMac: 'AA:BB:CC' });
    const { getByText } = render(<BluetoothDeviceListItem deviceName="RGB" macAddress="AA:BB:CC" />);
    expect(getByText('Disconnect')).toBeTruthy();
  });

  it('button is disabled while isConnecting', () => {
    setupMocks({ isConnecting: true });
    const { getByRole } = render(<BluetoothDeviceListItem deviceName="RGB" macAddress="AA:BB:CC" />);
    expect(getByRole('button', { name: 'Connect' }).props.accessibilityState?.disabled).toBe(true);
  });

  it('pressing Connect calls connect() then navigates', async () => {
    const { connect } = setupMocks({ selectedMac: null });
    const { getByText } = render(<BluetoothDeviceListItem deviceName="RGB" macAddress="AA:BB:CC" />);

    fireEvent.press(getByText('Connect'));

    await waitFor(() => {
      expect(connect).toHaveBeenCalledTimes(1);
      expect(mockRouter.navigate).toHaveBeenCalledWith('/(tabs)/device-state');
    });
  });

  it('pressing Disconnect calls disconnect() without navigating', async () => {
    const { disconnect } = setupMocks({ selectedMac: 'AA:BB:CC' });
    const { getByText } = render(<BluetoothDeviceListItem deviceName="RGB" macAddress="AA:BB:CC" />);

    fireEvent.press(getByText('Disconnect'));

    await waitFor(() => {
      expect(disconnect).toHaveBeenCalledTimes(1);
      expect(mockRouter.navigate).not.toHaveBeenCalled();
    });
  });

  it('shows ActivityIndicator when isConnecting', () => {
    setupMocks({ isConnecting: true });
    const { UNSAFE_getByType } = render(<BluetoothDeviceListItem deviceName="RGB" macAddress="AA:BB:CC" />);
    const { ActivityIndicator } = require('react-native');
    expect(UNSAFE_getByType(ActivityIndicator)).toBeTruthy();
  });
});