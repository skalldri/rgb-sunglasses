import { fireEvent, waitFor } from '@testing-library/react-native';
import React from 'react';
import * as BluetoothContext from '@/context/bluetooth-context';

import FirmwareUpdateDebug from '@/app/firmware-update/debug';
import { McubootUpdaterClient, McubootUpdaterState, McubootPackageInfo } from '@/services/mcuboot-updater-client';
import * as McubootUpdaterModule from '@/services/mcuboot-updater-client';
import * as GitHubReleases from '@/services/github-releases';
import * as DocumentPicker from 'expo-document-picker';
import * as LegacyFS from 'expo-file-system/legacy';
import { File } from 'expo-file-system/next';
import {
    defaultSelectedDevice,
    deviceWithMcubootVersion,
    mcubootRelease,
    mockBluetooth,
    mockClientMethods,
    mockGitHub,
    mockMcubootUpdater,
    renderWithMcuMgr,
} from '@/test/firmware-mocks';

jest.mock('expo-router', () => {
    const actual = jest.requireActual('expo-router');
    return { ...actual, useRouter: () => ({ push: jest.fn(), back: jest.fn() }) };
});

describe('FirmwareUpdateDebug', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
    jest.spyOn(console, 'warn').mockImplementation(() => {});
    jest.spyOn(console, 'error').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('renders the raw slot table', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods();
    const { findByText } = renderWithMcuMgr(<FirmwareUpdateDebug />);
    expect(await findByText('Current Images')).toBeTruthy();
    expect(await findByText('Image 0 / Slot 1')).toBeTruthy();
  });

  it('exposes the low-level device actions', async () => {
    mockBluetooth(defaultSelectedDevice);
    const spies = mockClientMethods();
    const { findByText } = renderWithMcuMgr(<FirmwareUpdateDebug />);

    fireEvent.press(await findByText('Reset Device'));
    await waitFor(() => expect(spies.reset).toHaveBeenCalled());

    fireEvent.press(await findByText('Erase Slot 1'));
    await waitFor(() => expect(spies.eraseImage).toHaveBeenCalledWith(1));

    fireEvent.press(await findByText('Mark for Test'));
    await waitFor(() =>
      expect(spies.setImageState).toHaveBeenCalledWith(expect.any(Uint8Array), false)
    );
  });

  // Every test in this block connects a proto0 device (mockClientMethods()'s default getOsInfo),
  // so both auto-update-check effects (firmware and bootloader) fire regardless of what the test
  // itself cares about. Node's built-in global fetch is real in this test environment, so leaving
  // either call unmocked triggers a genuine (and here, network-less-sandbox-hanging) HTTP request
  // that bleeds slow timing into whichever test runs next - always mock both explicitly.
  function mockNoGithubReleases() {
    mockGitHub({ fetchLatestFirmwareRelease: async () => { throw new Error('not relevant to this test'); } });
    jest.spyOn(GitHubReleases, 'fetchLatestMcubootRelease').mockResolvedValue(null);
  }

  it('shows the current MCUboot Version, moved here from the Controls tab', async () => {
    mockBluetooth(deviceWithMcubootVersion('1.0.0+0'));
    mockClientMethods();
    mockMcubootUpdater();
    mockNoGithubReleases();

    const { findByText } = renderWithMcuMgr(<FirmwareUpdateDebug />);
    expect(await findByText('Bootloader Update (Advanced)')).toBeTruthy();
    expect(await findByText('MCUboot Version')).toBeTruthy();
    expect(await findByText('1.0.0+0')).toBeTruthy();
  });

  it('reflects a device that is already unlocked at connect time (regression: used to require a notification that never came)', async () => {
    mockBluetooth(deviceWithMcubootVersion('1.0.0+0'));
    mockClientMethods();
    mockMcubootUpdater({
      initialStatus: { state: McubootUpdaterState.LOCKED, progress: 0, errorCode: 0, flashUnlocked: true },
    });
    mockNoGithubReleases();

    const { findByText, queryByText } = renderWithMcuMgr(<FirmwareUpdateDebug />);
    expect(await findByText('Flash is unlocked — select a package to flash')).toBeTruthy();
    expect(queryByText('Prepare Device')).toBeNull();
  });

  it('keeps the "reconnect" message visible through the disconnect after Prepare Device (bug 3a)', async () => {
    let selectedDevice: any = defaultSelectedDevice;
    jest
      .spyOn(BluetoothContext, 'useBluetooth')
      .mockImplementation(() => ({ selectedDevice, setSelectedDevice: jest.fn() } as any));
    mockClientMethods();
    const { requestUpdaterRebootSpy } = mockMcubootUpdater();
    mockNoGithubReleases();

    const { findByText, queryByText, rerender } = renderWithMcuMgr(<FirmwareUpdateDebug />);
    fireEvent.press(await findByText('Prepare Device'));

    await waitFor(() => {
      expect(requestUpdaterRebootSpy).toHaveBeenCalledTimes(1);
    });
    expect(await findByText('Device is rebooting — please reconnect after ~15 seconds')).toBeTruthy();

    // Simulate the BLE disconnect that follows the reboot ~200ms later — this used to hide the
    // whole bootloader section (and the message with it) because blUpdaterRef.current was
    // nulled and the render gate didn't account for blRebooting.
    selectedDevice = null;
    rerender(<FirmwareUpdateDebug />);

    expect(queryByText('Device is rebooting — please reconnect after ~15 seconds')).toBeTruthy();
  });

  describe('GitHub update check', () => {
    it('shows "Bootloader Update Available" when a newer mcuboot-v release exists', async () => {
      mockBluetooth(deviceWithMcubootVersion('1.0.0+0'));
      mockClientMethods();
      mockMcubootUpdater();
      mockGitHub({ fetchLatestFirmwareRelease: async () => { throw new Error('not relevant to this test'); } });
      jest.spyOn(GitHubReleases, 'fetchLatestMcubootRelease').mockResolvedValue(mcubootRelease);

      const { findByText } = renderWithMcuMgr(<FirmwareUpdateDebug />);
      expect(await findByText('Bootloader Update Available')).toBeTruthy();
      expect(await findByText('Current: v1.0.0+0')).toBeTruthy();
      expect(await findByText('Latest: v2.0.0')).toBeTruthy();
    });

    it('shows nothing extra when already up to date', async () => {
      mockBluetooth(deviceWithMcubootVersion('2.0.0'));
      mockClientMethods();
      mockMcubootUpdater();
      mockGitHub({ fetchLatestFirmwareRelease: async () => { throw new Error('not relevant to this test'); } });
      jest.spyOn(GitHubReleases, 'fetchLatestMcubootRelease').mockResolvedValue(mcubootRelease);

      const { findByText, queryByText } = renderWithMcuMgr(<FirmwareUpdateDebug />);
      expect(await findByText('Bootloader Update (Advanced)')).toBeTruthy();
      await waitFor(() => {
        expect(queryByText('Bootloader Update Available')).toBeNull();
      });
    });

    it('does not check GitHub when no MCUboot release has ever been published', async () => {
      mockBluetooth(deviceWithMcubootVersion('1.0.0+0'));
      mockClientMethods();
      mockMcubootUpdater();
      mockGitHub({ fetchLatestFirmwareRelease: async () => { throw new Error('not relevant to this test'); } });
      const fetchSpy = jest.spyOn(GitHubReleases, 'fetchLatestMcubootRelease').mockResolvedValue(null);

      const { findByText, queryByText } = renderWithMcuMgr(<FirmwareUpdateDebug />);
      expect(await findByText('Bootloader Update (Advanced)')).toBeTruthy();
      await waitFor(() => {
        expect(fetchSpy).toHaveBeenCalledTimes(1);
      });
      expect(queryByText('Bootloader Update Available')).toBeNull();
    });

    it('"Download Update" downloads the .bin asset and loads it as the flash-ready package', async () => {
      mockBluetooth(deviceWithMcubootVersion('1.0.0+0'));
      mockClientMethods();
      mockMcubootUpdater({
        initialStatus: { state: McubootUpdaterState.LOCKED, progress: 0, errorCode: 0, flashUnlocked: true },
      });
      mockGitHub({ fetchLatestFirmwareRelease: async () => { throw new Error('not relevant to this test'); } });
      jest.spyOn(GitHubReleases, 'fetchLatestMcubootRelease').mockResolvedValue(mcubootRelease);

      (File as unknown as jest.Mock).mockImplementation(() => ({
        base64: jest.fn(async () => btoa('raw-package-bytes')),
      }));
      const downloadedPackage: McubootPackageInfo = {
        major: 2,
        minor: 0,
        revision: 0,
        payloadSize: 4,
        crc32: 0,
        payload: new Uint8Array([1, 2, 3, 4]),
      };
      jest.spyOn(McubootUpdaterModule, 'parseMcubootPackage').mockReturnValue(downloadedPackage);

      const { findByText } = renderWithMcuMgr(<FirmwareUpdateDebug />);
      fireEvent.press(await findByText('Download Update'));

      await waitFor(() => {
        expect(LegacyFS.createDownloadResumable).toHaveBeenCalledWith(
          'https://example.com/mcuboot-2.0.0-proto0.bin',
          'file:///cache/mcuboot-update.bin',
          {},
          expect.any(Function)
        );
      });
      expect(await findByText('Package loaded')).toBeTruthy();
      expect(await findByText('Version: 2.0.0')).toBeTruthy();
    });
  });
});
