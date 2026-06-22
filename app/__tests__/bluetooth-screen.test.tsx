import React from 'react';
import { act, render, waitFor } from '@testing-library/react-native';
import * as ExpoRouter from 'expo-router';

import BluetoothScreen from '@/app/(tabs)/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import * as BleHook from '@/hooks/ble-manager';

jest.mock('@/components/bluetooth-device-list-item', () => {
  const React = require('react');
  const { Text } = require('react-native');
  return function MockBluetoothDeviceListItem({
    deviceName,
    macAddress,
  }: {
    deviceName: string;
    macAddress: string;
  }) {
    return <Text>{`${deviceName}|${macAddress}`}</Text>;
  };
});

describe('BluetoothScreen', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('scans, filters RGB devices, and de-duplicates results from scan and connected list', async () => {
    const setIsScanning = jest.fn();
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      isScanning: false,
      setIsScanning,
    } as any);

    let focusCallback: (() => void | (() => void)) | null = null;
    jest.spyOn(ExpoRouter, 'useFocusEffect').mockImplementation((cb: () => void | (() => void)) => {
      focusCallback = cb;
      return undefined as any;
    });

    jest.spyOn(BleHook, 'requestPermissions').mockResolvedValue(true);
    const startScanMock = BleHook.bleManager.startDeviceScan as jest.Mock;
    startScanMock.mockImplementation((_filters, _options, callback) => {
      callback(null, { localName: 'RGB Sunglasses A', id: 'id-1' });
      callback(null, { localName: 'RGB Sunglasses A', id: 'id-1' }); // duplicate
      callback(null, { localName: 'Other Device', id: 'id-2' }); // filtered out
    });

    (BleHook.bleManager.connectedDevices as jest.Mock).mockResolvedValue([
      { id: 'id-1', localName: 'RGB Sunglasses A', name: 'RGB Sunglasses A' }, // duplicate
      { id: 'id-3', localName: 'RGB Sunglasses B', name: 'RGB Sunglasses B' },
    ]);

    const { findByText, queryByText } = render(<BluetoothScreen />);
    await act(async () => {
      focusCallback?.();
    });

    expect(await findByText('RGB Sunglasses A|id-1')).toBeTruthy();
    expect(await findByText('RGB Sunglasses B|id-3')).toBeTruthy();
    expect(queryByText('Other Device|id-2')).toBeNull();

    await waitFor(() => {
      expect(startScanMock).toHaveBeenCalled();
      expect(BleHook.bleManager.connectedDevices).toHaveBeenCalledWith([
        '12345678-1234-5678-0001-56789abc0000',
      ]);
    });
    expect(setIsScanning).toHaveBeenCalledWith(true);
  });

  it('stops scanning on focus cleanup', async () => {
    const setIsScanning = jest.fn();
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      isScanning: false,
      setIsScanning,
    } as any);

    let focusCallback: (() => (() => void) | void) | null = null;
    jest.spyOn(ExpoRouter, 'useFocusEffect').mockImplementation((cb: () => (() => void) | void) => {
      focusCallback = cb;
      return undefined as any;
    });

    jest.spyOn(BleHook, 'requestPermissions').mockResolvedValue(true);
    (BleHook.bleManager.startDeviceScan as jest.Mock).mockImplementation(() => undefined);
    (BleHook.bleManager.connectedDevices as jest.Mock).mockResolvedValue([]);

    render(<BluetoothScreen />);

    let cleanup: (() => void) | void;
    await act(async () => {
      cleanup = focusCallback?.();
    });
    await act(async () => {
      cleanup?.();
    });

    await waitFor(() => {
      expect(BleHook.bleManager.stopDeviceScan).toHaveBeenCalledTimes(1);
    });
    expect(setIsScanning).toHaveBeenCalledWith(false);
  });

  it('continues scan flow even when permission request resolves false', async () => {
    const setIsScanning = jest.fn();
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      isScanning: false,
      setIsScanning,
    } as any);

    let focusCallback: (() => void | (() => void)) | null = null;
    jest.spyOn(ExpoRouter, 'useFocusEffect').mockImplementation((cb: () => void | (() => void)) => {
      focusCallback = cb;
      return undefined as any;
    });

    jest.spyOn(BleHook, 'requestPermissions').mockResolvedValue(false);
    (BleHook.bleManager.startDeviceScan as jest.Mock).mockImplementation(() => undefined);
    (BleHook.bleManager.connectedDevices as jest.Mock).mockResolvedValue([]);

    render(<BluetoothScreen />);
    await act(async () => {
      focusCallback?.();
    });

    await waitFor(() => {
      expect(BleHook.bleManager.startDeviceScan).toHaveBeenCalled();
    });
  });
});
