/**
 * Shared mocks for the firmware-update screens.
 *
 * Lives in `test/` rather than `__tests__/` because jest treats everything under
 * `__tests__/` as a suite and would fail this file for containing no tests.
 *
 * All four factories target module/prototype exports rather than component internals,
 * which is why they survived the split of the old single-screen modal into the
 * landing / flow / debug / extensions screens.
 */
import React from 'react';
import { render } from '@testing-library/react-native';
import { McuMgrClientProvider } from '@/context/mcumgr-client-context';
import { sha256 } from 'js-sha256';

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

export const mockRelease: GitHubReleases.GitHubRelease = {
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

export type MockClientSpies = {
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

export const defaultSelectedDevice = {
  name: 'RGB Sunglasses',
  mac: 'AA:BB:CC',
  device: {},
};

export function mockBluetooth(selectedDevice: any, setSelectedDevice = jest.fn()) {
  jest
    .spyOn(BluetoothContext, 'useBluetooth')
    .mockReturnValue({ selectedDevice, setSelectedDevice } as any);
  return setSelectedDevice;
}

export function mockClientMethods(overrides?: Partial<Record<keyof MockClientSpies, any>>): MockClientSpies {
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

export function mockGitHub(overrides?: { fetchLatestFirmwareRelease?: any }) {
  jest
    .spyOn(GitHubReleases, 'fetchLatestFirmwareRelease')
    .mockImplementation(overrides?.fetchLatestFirmwareRelease ?? (async () => mockRelease));
}

export const mcubootRelease: GitHubReleases.GitHubRelease = {
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
export function deviceWithMcubootVersion(version: string) {
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
export function mockMcubootUpdater(overrides?: {
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


/**
 * Renders a firmware screen inside the real `McuMgrClientProvider`.
 *
 * Deliberately the real provider, not a mocked context: it calls `useMcuMgrClient`,
 * which constructs a `McuMgrClient` that `mockClientMethods()` has already
 * prototype-spied. That keeps these tests exercising the same client lifecycle the
 * app uses - including the `setSelectedDevice({...,mcuMgrClient})` patch the provider
 * now owns - instead of a stub that could drift from it.
 */
export function renderWithMcuMgr(ui: React.ReactElement) {
    const result = render(<McuMgrClientProvider>{ui}</McuMgrClientProvider>);
    return {
        ...result,
        // Re-wrap on rerender, so the "fresh context identities" regression tests
        // re-render the SAME tree rather than mounting a new one (a new tree would
        // re-run every effect and hide exactly the re-fire this guards against).
        rerender: (next: React.ReactElement) =>
            result.rerender(<McuMgrClientProvider>{next}</McuMgrClientProvider>),
    };
}
