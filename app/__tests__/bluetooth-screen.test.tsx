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
    // bleManager is a module-level singleton (see jest.setup.ts's
    // react-native-ble-plx mock) shared across every test in this file -
    // restoreAllMocks() (afterEach, below) only reverts jest.spyOn overrides,
    // it does not clear a jest.fn()'s accumulated .mock.calls, so without this
    // a later test's toHaveBeenCalledTimes() assertion would count calls left
    // over from earlier tests too.
    jest.clearAllMocks();
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

  it('scans in LowLatency mode (not the LowPower default that misses late advertisers)', async () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      isScanning: false,
      setIsScanning: jest.fn(),
    } as any);
    let focusCallback: (() => void | (() => void)) | null = null;
    jest.spyOn(ExpoRouter, 'useFocusEffect').mockImplementation((cb: () => void | (() => void)) => {
      focusCallback = cb;
      return undefined as any;
    });
    jest.spyOn(BleHook, 'requestPermissions').mockResolvedValue(true);
    const startScanMock = BleHook.bleManager.startDeviceScan as jest.Mock;
    startScanMock.mockImplementation(() => undefined);
    (BleHook.bleManager.connectedDevices as jest.Mock).mockResolvedValue([]);

    render(<BluetoothScreen />);
    await act(async () => {
      focusCallback?.();
    });

    await waitFor(() => expect(startScanMock).toHaveBeenCalled());
    // 2nd arg is the ScanOptions; LowLatency = 2 (ScanMode.LowLatency).
    expect(startScanMock.mock.calls[0][1]).toEqual({ scanMode: 2 });
  });

  it('periodically restarts the scan to defeat Android scan-report de-dup', async () => {
    jest.useFakeTimers();
    try {
      jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
        isScanning: true,
        setIsScanning: jest.fn(),
      } as any);
      let focusCallback: (() => void | (() => void)) | null = null;
      jest.spyOn(ExpoRouter, 'useFocusEffect').mockImplementation((cb: () => void | (() => void)) => {
        focusCallback = cb;
        return undefined as any;
      });
      jest.spyOn(BleHook, 'requestPermissions').mockResolvedValue(true);
      const startScanMock = BleHook.bleManager.startDeviceScan as jest.Mock;
      startScanMock.mockImplementation(() => undefined);
      const stopScanMock = BleHook.bleManager.stopDeviceScan as jest.Mock;
      (BleHook.bleManager.connectedDevices as jest.Mock).mockResolvedValue([]);

      render(<BluetoothScreen />);
      await act(async () => {
        focusCallback?.();
      });

      const startsAfterInitial = startScanMock.mock.calls.length; // initial scan (1)
      const stopsAfterInitial = stopScanMock.mock.calls.length;

      // One rescan interval later, the scan is stop+restarted (no focus change).
      act(() => {
        jest.advanceTimersByTime(11000); // > RESCAN_INTERVAL_MS (10s)
      });

      expect(startScanMock.mock.calls.length).toBeGreaterThan(startsAfterInitial);
      expect(stopScanMock.mock.calls.length).toBeGreaterThan(stopsAfterInitial);
    } finally {
      jest.useRealTimers();
    }
  });

  it('prunes a device that stops advertising after the TTL, keeps one that keeps advertising', async () => {
    jest.useFakeTimers();
    try {
      jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
        isScanning: true,
        setIsScanning: jest.fn(),
      } as any);
      let focusCallback: (() => void | (() => void)) | null = null;
      jest.spyOn(ExpoRouter, 'useFocusEffect').mockImplementation((cb: () => void | (() => void)) => {
        focusCallback = cb;
        return undefined as any;
      });
      jest.spyOn(BleHook, 'requestPermissions').mockResolvedValue(true);
      // Capture the scan callback WITHOUT auto-reporting, so the test fully controls which
      // device is "re-advertised" (the periodic rescan re-registers the callback, so an
      // auto-reporting mock would refresh every device and nothing would ever prune).
      let scanCb: ((error: unknown, device: unknown) => void) | null = null;
      (BleHook.bleManager.startDeviceScan as jest.Mock).mockImplementation((_f, _o, cb) => {
        scanCb = cb;
      });
      (BleHook.bleManager.connectedDevices as jest.Mock).mockResolvedValue([]);

      const { queryByText } = render(<BluetoothScreen />);
      await act(async () => {
        focusCallback?.();
      });

      // Both discovered at t=0.
      act(() => {
        scanCb?.(null, { localName: 'RGB Sunglasses Stays', id: 'stays' });
        scanCb?.(null, { localName: 'RGB Sunglasses Goes', id: 'goes' });
      });
      expect(queryByText('RGB Sunglasses Stays|stays')).toBeTruthy();
      expect(queryByText('RGB Sunglasses Goes|goes')).toBeTruthy();

      // Advance well past DEVICE_TTL_MS (25s), re-advertising ONLY 'stays' each step so its
      // lastSeen stays fresh; 'goes' is never re-reported and its lastSeen stays at t=0.
      for (let i = 0; i < 6; i++) {
        act(() => {
          scanCb?.(null, { localName: 'RGB Sunglasses Stays', id: 'stays' });
          jest.advanceTimersByTime(6000); // 6 x 6s = 36s (> 25s TTL); prune fires each 2s
        });
      }

      // 'goes' aged out; 'stays' kept because it kept advertising.
      expect(queryByText('RGB Sunglasses Goes|goes')).toBeNull();
      expect(queryByText('RGB Sunglasses Stays|stays')).toBeTruthy();
    } finally {
      jest.useRealTimers();
    }
  });

  it('keeps the connecting/pairing device even after it stops advertising, still prunes others (issue #158)', async () => {
    jest.useFakeTimers();
    try {
      // A board being paired stops advertising the moment its LE link comes up, so its lastSeen
      // freezes for the whole pairing+discovery phase. connectingDevice pins it so the prune timer
      // can't age it out mid-pair (which used to unmount its in-progress "Querying characteristics"
      // row and make the pair look failed). A different device that stops advertising still prunes.
      jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
        isScanning: true,
        setIsScanning: jest.fn(),
        connectingDevice: { mac: 'pairing', name: 'RGB Sunglasses Pairing' },
      } as any);
      let focusCallback: (() => void | (() => void)) | null = null;
      jest.spyOn(ExpoRouter, 'useFocusEffect').mockImplementation((cb: () => void | (() => void)) => {
        focusCallback = cb;
        return undefined as any;
      });
      jest.spyOn(BleHook, 'requestPermissions').mockResolvedValue(true);
      // Capture the scan callback without auto-reporting, so nothing is re-advertised on its own.
      let scanCb: ((error: unknown, device: unknown) => void) | null = null;
      (BleHook.bleManager.startDeviceScan as jest.Mock).mockImplementation((_f, _o, cb) => {
        scanCb = cb;
      });
      (BleHook.bleManager.connectedDevices as jest.Mock).mockResolvedValue([]);

      const { queryByText } = render(<BluetoothScreen />);
      await act(async () => {
        focusCallback?.();
      });

      // Both seen once at t=0 (their last advertisement); neither is re-reported after.
      act(() => {
        scanCb?.(null, { localName: 'RGB Sunglasses Pairing', id: 'pairing' });
        scanCb?.(null, { localName: 'RGB Sunglasses Other', id: 'other' });
      });
      expect(queryByText('RGB Sunglasses Pairing|pairing')).toBeTruthy();
      expect(queryByText('RGB Sunglasses Other|other')).toBeTruthy();

      // Advance well past DEVICE_TTL_MS (25s) without re-advertising either.
      act(() => {
        jest.advanceTimersByTime(40000);
      });

      // The connecting device is pinned and stays; the other one ages out normally.
      expect(queryByText('RGB Sunglasses Pairing|pairing')).toBeTruthy();
      expect(queryByText('RGB Sunglasses Other|other')).toBeNull();
    } finally {
      jest.useRealTimers();
    }
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

    // 2 calls: startBluetoothScan's own defensive stopDeviceScan() (disposes
    // any scan orphaned by a prior focus-effect race, see bluetooth.tsx) plus
    // the one from this cleanup itself.
    await waitFor(() => {
      expect(BleHook.bleManager.stopDeviceScan).toHaveBeenCalledTimes(2);
    });
    expect(setIsScanning).toHaveBeenCalledWith(false);
  });

  it('stops the scan flow gracefully when permission request resolves false', async () => {
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

    // Permission denial hits startBluetoothScan's early return - it must never
    // reach the real bleManager.startDeviceScan() call, just stop cleanly.
    await waitFor(() => {
      expect(setIsScanning).toHaveBeenCalledWith(false);
    });
    expect(BleHook.bleManager.startDeviceScan).not.toHaveBeenCalled();
  });
});
