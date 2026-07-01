import React from 'react';
import { fireEvent, render, waitFor } from '@testing-library/react-native';

import FirmwareUpdateModal from '@/app/firmware-update-modal';
import { UUID_MCUBOOT_INFO_SERVICE } from '@/constants/bluetooth';
import * as BluetoothContext from '@/context/bluetooth-context';
import { encodeUtf8ToBase64 } from '@/services/ble-value-codec';
import * as FirmwarePackageService from '@/services/firmware-package';
import * as GitHubReleases from '@/services/github-releases';
import { McubootUpdaterClient, McubootUpdaterState, McubootPackageInfo } from '@/services/mcuboot-updater-client';
import * as McubootUpdaterModule from '@/services/mcuboot-updater-client';
import * as McuMgrModule from '@/services/mcumgr';
import * as DocumentPicker from 'expo-document-picker';
import * as LegacyFS from 'expo-file-system/legacy';
import { File } from 'expo-file-system/next';

const mockRelease: GitHubReleases.GitHubRelease = {
  id: 1,
  tag_name: 'fw-v2.0.0',
  name: 'Firmware v2.0.0',
  published_at: '2026-01-01T00:00:00Z',
  assets: [
    {
      id: 10,
      name: 'firmware_proto0_v2.0.0.zip',
      browser_download_url: 'https://example.com/firmware_proto0_v2.0.0.zip',
      size: 512000,
      content_type: 'application/zip',
    },
    {
      id: 11,
      name: 'firmware_dk_v2.0.0.zip',
      browser_download_url: 'https://example.com/firmware_dk_v2.0.0.zip',
      size: 512000,
      content_type: 'application/zip',
    },
  ],
};

type MockClientSpies = {
  initialize: jest.SpyInstance;
  getImageState: jest.SpyInstance;
  getSlotInfo: jest.SpyInstance;
  uploadImage: jest.SpyInstance;
  setImageState: jest.SpyInstance;
  reset: jest.SpyInstance;
  eraseImage: jest.SpyInstance;
  destroy: jest.SpyInstance;
  getOsInfo: jest.SpyInstance;
};

const defaultSelectedDevice = {
  name: 'RGB Sunglasses',
  mac: 'AA:BB:CC',
  device: {},
};

function mockBluetooth(selectedDevice: any, setSelectedDevice = jest.fn()) {
  jest
    .spyOn(BluetoothContext, 'useBluetooth')
    .mockReturnValue({ selectedDevice, setSelectedDevice } as any);
  return setSelectedDevice;
}

function mockClientMethods(overrides?: Partial<Record<keyof MockClientSpies, any>>): MockClientSpies {
  return {
    initialize: jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'initialize')
      .mockImplementation(overrides?.initialize ?? (async () => undefined)),
    getImageState: jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'getImageState')
      .mockImplementation(
        overrides?.getImageState ??
          (async () => ({
            images: [{ image: 0, slot: 1, version: '1.0.0', hash: Uint8Array.from([1, 2, 3]) }],
          }))
      ),
    getSlotInfo: jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'getSlotInfo')
      .mockImplementation(
        overrides?.getSlotInfo ??
          (async () => ({
            images: [{ image: 0, slots: [{ slot: 0, size: 1024 }] }],
          }))
      ),
    uploadImage: jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'uploadImage')
      .mockImplementation(
        overrides?.uploadImage ??
          (async (_data: Uint8Array, _index: number, onProgress?: (sent: number, total: number) => void) => {
            onProgress?.(10, 10);
          })
      ),
    setImageState: jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'setImageState')
      .mockImplementation(overrides?.setImageState ?? (async () => ({ images: [] }))),
    reset: jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'reset')
      .mockImplementation(overrides?.reset ?? (async () => undefined)),
    eraseImage: jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'eraseImage')
      .mockImplementation(overrides?.eraseImage ?? (async () => undefined)),
    destroy: jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'destroy')
      .mockImplementation(overrides?.destroy ?? (() => undefined)),
    getOsInfo: jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'getOsInfo')
      .mockImplementation(overrides?.getOsInfo ?? (async () => 'rgb_sunglasses_proto0_nrf5340_cpuapp')),
  };
}

function mockGitHub(overrides?: { fetchLatestFirmwareRelease?: any }) {
  jest
    .spyOn(GitHubReleases, 'fetchLatestFirmwareRelease')
    .mockImplementation(overrides?.fetchLatestFirmwareRelease ?? (async () => mockRelease));
}

const mcubootRelease: GitHubReleases.GitHubRelease = {
  id: 2,
  tag_name: 'mcuboot-v2.0.0',
  name: 'MCUboot v2.0.0',
  published_at: '2026-01-01T00:00:00Z',
  assets: [
    {
      id: 20,
      name: 'mcuboot-2.0.0-proto0.bin',
      browser_download_url: 'https://example.com/mcuboot-2.0.0-proto0.bin',
      size: 4096,
      content_type: 'application/octet-stream',
    },
  ],
};

/** A selectedDevice with a live "MCUboot Version" characteristic, as populated by BluetoothContext. */
function deviceWithMcubootVersion(version: string) {
  const versionCharUuid = '12345678-1234-5678-0003-56789abc0001';
  return {
    ...defaultSelectedDevice,
    characteristicsByService: {
      [UUID_MCUBOOT_INFO_SERVICE]: {
        [versionCharUuid]: {
          characteristic: {},
          value: encodeUtf8ToBase64(version),
          name: 'MCUboot Version',
          cpfFormat: 0x19,
          isUpdateInProgress: false,
        },
      },
    },
  };
}

/**
 * Mocks McubootUpdaterClient at the prototype level (same approach as McuMgrClient above).
 * By default, initialize() simulates the real client's behaviour post-fix: it delivers an
 * initial status via whatever handler was registered through onStatusChanged *before*
 * initialize() was awaited — reproducing the real ordering dependency the issue #76 fix relies on
 * in firmware-update-modal.tsx (onStatusChanged is now called before initialize(), not after).
 */
function mockMcubootUpdater(overrides?: {
  initialStatus?: { state: McubootUpdaterState; progress: number; errorCode: number; flashUnlocked: boolean };
  initialize?: any;
}) {
  let statusHandler: ((s: any) => void) | null = null;
  const onStatusChangedSpy = jest
    .spyOn(McubootUpdaterClient.prototype, 'onStatusChanged')
    .mockImplementation(function (this: any, handler: any) {
      statusHandler = handler;
    });
  const initialize =
    overrides?.initialize ??
    (async () => {
      statusHandler?.(
        overrides?.initialStatus ?? {
          state: McubootUpdaterState.LOCKED,
          progress: 0,
          errorCode: 0,
          flashUnlocked: false,
        }
      );
    });
  const initializeSpy = jest.spyOn(McubootUpdaterClient.prototype, 'initialize').mockImplementation(initialize);
  const destroySpy = jest.spyOn(McubootUpdaterClient.prototype, 'destroy').mockImplementation(() => undefined);
  const requestUpdaterRebootSpy = jest
    .spyOn(McubootUpdaterClient.prototype, 'requestUpdaterReboot')
    .mockImplementation(async () => undefined);
  return {
    onStatusChangedSpy,
    initializeSpy,
    destroySpy,
    requestUpdaterRebootSpy,
    emitStatus: (s: any) => statusHandler?.(s),
  };
}

describe('FirmwareUpdateModal', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
    jest.spyOn(console, 'warn').mockImplementation(() => {});
    jest.spyOn(console, 'error').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('shows an initialization error when no device is connected', async () => {
    mockBluetooth(null);
    const { findByText } = render(<FirmwareUpdateModal />);
    expect(await findByText('No device connected')).toBeTruthy();
  });

  it('initializes MCUmgr client and renders current images', async () => {
    const setSelectedDevice = mockBluetooth(defaultSelectedDevice);
    const spies = mockClientMethods();

    const { findByText } = render(<FirmwareUpdateModal />);

    expect(await findByText('Current Images')).toBeTruthy();
    expect(await findByText('Image 0 / Slot 1')).toBeTruthy();
    expect(spies.initialize).toHaveBeenCalledTimes(1);
    expect(setSelectedDevice).toHaveBeenCalledWith(
      expect.objectContaining({ mcuMgrClient: expect.any(McuMgrModule.McuMgrClient) })
    );
  });

  it('shows initialization failure when client init throws', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods({
      initialize: async () => {
        throw new Error('init failed');
      },
    });

    const { findByText } = render(<FirmwareUpdateModal />);
    expect(await findByText('Failed to initialize: init failed')).toBeTruthy();
  });

  it('loads firmware package via parser when a zip is selected', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods();

    const getDocumentAsyncMock = DocumentPicker.getDocumentAsync as jest.Mock;
    getDocumentAsyncMock.mockResolvedValueOnce({
      canceled: false,
      assets: [{ name: 'firmware.zip', uri: 'file:///firmware.zip' }],
    });

    const fileCtorMock = File as unknown as jest.Mock;
    fileCtorMock.mockImplementation(() => ({
      base64: jest.fn(async () => 'base64-zip-data'),
    }));

    jest
      .spyOn(FirmwarePackageService, 'parseFirmwarePackageFromBase64')
      .mockResolvedValueOnce({
        manifest: { name: 'Test Package', files: [], time: 0, 'format-version': 1 },
        images: [],
      });

    const { findByText } = render(<FirmwareUpdateModal />);
    const selectButton = await findByText('Select Firmware Package (.zip)');
    fireEvent.press(selectButton);

    await waitFor(() => {
      expect(FirmwarePackageService.parseFirmwarePackageFromBase64).toHaveBeenCalledWith('base64-zip-data');
    });
    expect(await findByText('Firmware Package: Test Package')).toBeTruthy();
  });

  it('shows package parsing error when parser throws', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods();

    (DocumentPicker.getDocumentAsync as jest.Mock).mockResolvedValueOnce({
      canceled: false,
      assets: [{ name: 'bad.zip', uri: 'file:///bad.zip' }],
    });
    (File as unknown as jest.Mock).mockImplementation(() => ({
      base64: jest.fn(async () => 'bad-base64'),
    }));
    jest
      .spyOn(FirmwarePackageService, 'parseFirmwarePackageFromBase64')
      .mockRejectedValueOnce(new Error('No manifest.json found in firmware package'));

    const { findByText } = render(<FirmwareUpdateModal />);
    fireEvent.press(await findByText('Select Firmware Package (.zip)'));

    expect(
      await findByText('Failed to load firmware package: No manifest.json found in firmware package')
    ).toBeTruthy();
  });

  it('runs update flow for a loaded package and confirms uploaded images', async () => {
    mockBluetooth(defaultSelectedDevice);
    const spies = mockClientMethods({
      getImageState: jest
        .fn()
        .mockResolvedValueOnce({ images: [{ slot: 1, image: 0, version: 'init', hash: Uint8Array.from([9]) }] }) // init
        .mockResolvedValueOnce({ images: [{ slot: 1, image: 0, version: '1.0.0', hash: Uint8Array.from([1]) }] })
        .mockResolvedValueOnce({ images: [{ slot: 1, image: 1, version: '2.0.0', hash: Uint8Array.from([2]) }] })
        .mockResolvedValueOnce({ images: [] }) // refreshImageState after upload loop
        .mockResolvedValue({ images: [] }),
      uploadImage: jest.fn(async (_data: Uint8Array, _index: number, onProgress?: (s: number, t: number) => void) => {
        onProgress?.(5, 10);
        onProgress?.(10, 10);
      }),
      setImageState: jest.fn(async () => ({ images: [] })),
    });

    (DocumentPicker.getDocumentAsync as jest.Mock).mockResolvedValueOnce({
      canceled: false,
      assets: [{ name: 'firmware.zip', uri: 'file:///firmware.zip' }],
    });
    (File as unknown as jest.Mock).mockImplementation(() => ({
      base64: jest.fn(async () => 'base64-zip-data'),
    }));
    jest
      .spyOn(FirmwarePackageService, 'parseFirmwarePackageFromBase64')
      .mockResolvedValueOnce({
        manifest: { name: 'Two Images', files: [], time: 0, 'format-version': 1 },
        images: [
          { manifest: { image_index: '0', file: 'app.bin' } as any, data: new Uint8Array([1]), parsedHeader: null },
          { manifest: { image_index: '1', file: 'radio.bin' } as any, data: new Uint8Array([2]), parsedHeader: null },
        ],
      });

    const { findByText } = render(<FirmwareUpdateModal />);
    fireEvent.press(await findByText('Select Firmware Package (.zip)'));
    fireEvent.press(await findByText('Start Update'));

    await waitFor(() => {
      expect(spies.uploadImage).toHaveBeenCalledTimes(2);
    });
    expect(spies.setImageState).toHaveBeenNthCalledWith(1, Uint8Array.from([1]), true);
    expect(spies.setImageState).toHaveBeenNthCalledWith(2, Uint8Array.from([2]), true);
    expect(await findByText('Select Firmware Package (.zip)')).toBeTruthy();
  });

  it('shows upload failure when uploaded image cannot be found', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods({
      uploadImage: async () => undefined,
      getImageState: async () => ({ images: [{ slot: 0, image: 0, version: '1.0.0' }] }),
    });

    (DocumentPicker.getDocumentAsync as jest.Mock).mockResolvedValueOnce({
      canceled: false,
      assets: [{ name: 'firmware.zip', uri: 'file:///firmware.zip' }],
    });
    (File as unknown as jest.Mock).mockImplementation(() => ({
      base64: jest.fn(async () => 'base64-zip-data'),
    }));
    jest
      .spyOn(FirmwarePackageService, 'parseFirmwarePackageFromBase64')
      .mockResolvedValueOnce({
        manifest: { name: 'One Image', files: [], time: 0, 'format-version': 1 },
        images: [
          { manifest: { image_index: '0', file: 'app.bin' } as any, data: new Uint8Array([1]), parsedHeader: null },
        ],
      });

    const { findByText } = render(<FirmwareUpdateModal />);
    fireEvent.press(await findByText('Select Firmware Package (.zip)'));
    fireEvent.press(await findByText('Start Update'));

    expect(await findByText('Upload failed: Uploaded image not found in image state response')).toBeTruthy();
  });

  describe('auto-update flow', () => {
    it('shows "Checking for updates" after board is detected', async () => {
      mockBluetooth(defaultSelectedDevice);
      mockClientMethods();
      // Hold the GitHub call so we can observe the intermediate state
      jest
        .spyOn(GitHubReleases, 'fetchLatestFirmwareRelease')
        .mockImplementation(() => new Promise(() => {})); // never resolves

      const { findByText } = render(<FirmwareUpdateModal />);
      expect(await findByText('Checking for updates...')).toBeTruthy();
    });

    it('shows "Up to date" when device version >= GitHub version', async () => {
      mockBluetooth(defaultSelectedDevice);
      mockClientMethods({
        getImageState: async () => ({
          images: [{ image: 0, slot: 0, version: '2.0.0', active: true, confirmed: true }],
        }),
      });
      mockGitHub();

      const { findByText } = render(<FirmwareUpdateModal />);
      expect(await findByText('Up to date (v2.0.0)')).toBeTruthy();
    });

    it('shows "Update Available" card when device version < GitHub version', async () => {
      mockBluetooth(defaultSelectedDevice);
      mockClientMethods({
        getImageState: async () => ({
          images: [{ image: 0, slot: 0, version: '1.0.0', active: true, confirmed: true }],
        }),
      });
      mockGitHub();

      const { findByText } = render(<FirmwareUpdateModal />);
      expect(await findByText('Update Available')).toBeTruthy();
      expect(await findByText('Current: v1.0.0')).toBeTruthy();
      expect(await findByText('Latest: v2.0.0')).toBeTruthy();
      expect(await findByText('Download Update')).toBeTruthy();
    });

    it('"Download Update" triggers download and auto-populates firmware package', async () => {
      mockBluetooth(defaultSelectedDevice);
      mockClientMethods({
        getImageState: async () => ({
          images: [{ image: 0, slot: 0, version: '1.0.0', active: true, confirmed: true }],
        }),
      });
      mockGitHub();

      (File as unknown as jest.Mock).mockImplementation(() => ({
        base64: jest.fn(async () => 'downloaded-base64'),
      }));
      jest
        .spyOn(FirmwarePackageService, 'parseFirmwarePackageFromBase64')
        .mockResolvedValueOnce({
          manifest: { name: 'Downloaded Package', files: [], time: 0, 'format-version': 1 },
          images: [],
        });

      const { findByText } = render(<FirmwareUpdateModal />);
      fireEvent.press(await findByText('Download Update'));

      await waitFor(() => {
        expect(LegacyFS.createDownloadResumable).toHaveBeenCalledWith(
          'https://example.com/firmware_proto0_v2.0.0.zip',
          'file:///cache/firmware-update.zip',
          {},
          expect.any(Function)
        );
      });
      expect(await findByText('Firmware Package: Downloaded Package')).toBeTruthy();
    });

    it('shows error when board detection fails but manual picker remains accessible', async () => {
      mockBluetooth(defaultSelectedDevice);
      mockClientMethods({
        getOsInfo: async () => { throw new Error('SMP timeout'); },
      });

      const { findByText } = render(<FirmwareUpdateModal />);
      expect(await findByText('Board detection failed: SMP timeout')).toBeTruthy();
      expect(await findByText('Select Firmware Package (.zip)')).toBeTruthy();
    });

    it('shows error when GitHub API fails', async () => {
      mockBluetooth(defaultSelectedDevice);
      mockClientMethods();
      mockGitHub({
        fetchLatestFirmwareRelease: async () => { throw new Error('Network error'); },
      });

      const { findByText } = render(<FirmwareUpdateModal />);
      expect(await findByText('Update check failed: Network error')).toBeTruthy();
    });

    it('clears the stale "Update Available" card once the device disconnects (e.g. reboot to apply an update)', async () => {
      // Simulates onDeviceDisconnected's setSelectedDevice(null) (use-ble-connection.ts) firing
      // mid-screen, e.g. after "Reset Device" reboots the board to apply an update. Without
      // resetting updateCheckState/boardRevision on client loss, this card would otherwise keep
      // showing a now-inaccurate version comparison until the user manually leaves and re-opens
      // this screen.
      let selectedDevice: any = defaultSelectedDevice;
      jest.spyOn(BluetoothContext, 'useBluetooth').mockImplementation(
        () => ({ selectedDevice, setSelectedDevice: jest.fn() } as any)
      );
      mockClientMethods({
        getImageState: async () => ({
          images: [{ image: 0, slot: 0, version: '1.0.0', active: true, confirmed: true }],
        }),
      });
      mockGitHub();

      const { findByText, queryByText, rerender } = render(<FirmwareUpdateModal />);
      expect(await findByText('Update Available')).toBeTruthy();

      selectedDevice = null;
      rerender(<FirmwareUpdateModal />);

      await waitFor(() => {
        expect(queryByText('Update Available')).toBeNull();
      });
    });
  });

  it('runs reset, erase, and mark-for-test actions', async () => {
    mockBluetooth(defaultSelectedDevice);
    const spies = mockClientMethods({
      getImageState: jest
        .fn()
        .mockResolvedValueOnce({
          images: [{ image: 0, slot: 1, version: '1.0.0', hash: Uint8Array.from([9]) }],
        })
        .mockResolvedValue({ images: [] }),
    });

    const { findByText } = render(<FirmwareUpdateModal />);

    fireEvent.press(await findByText('Mark for Test'));
    await waitFor(() => {
      expect(spies.setImageState).toHaveBeenCalledWith(Uint8Array.from([9]), false);
    });

    fireEvent.press(await findByText('Reset Device'));
    await waitFor(() => {
      expect(spies.reset).toHaveBeenCalledTimes(1);
    });

    fireEvent.press(await findByText('Erase Slot 1'));
    await waitFor(() => {
      expect(spies.eraseImage).toHaveBeenCalledWith(1);
    });
  });

  describe('bootloader section (issue #76)', () => {
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

      const { findByText } = render(<FirmwareUpdateModal />);
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

      const { findByText, queryByText } = render(<FirmwareUpdateModal />);
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

      const { findByText, queryByText, rerender } = render(<FirmwareUpdateModal />);
      fireEvent.press(await findByText('Prepare Device'));

      await waitFor(() => {
        expect(requestUpdaterRebootSpy).toHaveBeenCalledTimes(1);
      });
      expect(await findByText('Device is rebooting — please reconnect after ~15 seconds')).toBeTruthy();

      // Simulate the BLE disconnect that follows the reboot ~200ms later — this used to hide the
      // whole bootloader section (and the message with it) because blUpdaterRef.current was
      // nulled and the render gate didn't account for blRebooting.
      selectedDevice = null;
      rerender(<FirmwareUpdateModal />);

      expect(queryByText('Device is rebooting — please reconnect after ~15 seconds')).toBeTruthy();
    });

    describe('GitHub update check', () => {
      it('shows "Bootloader Update Available" when a newer mcuboot-v release exists', async () => {
        mockBluetooth(deviceWithMcubootVersion('1.0.0+0'));
        mockClientMethods();
        mockMcubootUpdater();
        mockGitHub({ fetchLatestFirmwareRelease: async () => { throw new Error('not relevant to this test'); } });
        jest.spyOn(GitHubReleases, 'fetchLatestMcubootRelease').mockResolvedValue(mcubootRelease);

        const { findByText } = render(<FirmwareUpdateModal />);
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

        const { findByText, queryByText } = render(<FirmwareUpdateModal />);
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

        const { findByText, queryByText } = render(<FirmwareUpdateModal />);
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

        const { findByText } = render(<FirmwareUpdateModal />);
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
});
