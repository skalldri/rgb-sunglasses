import { fireEvent, waitFor } from '@testing-library/react-native';
import React from 'react';
import { Alert } from 'react-native';
import * as BluetoothContext from '@/context/bluetooth-context';
import { sha256 } from 'js-sha256';

import ExtensionManagementScreen from '@/app/firmware-update/extensions';
import * as GitHubReleases from '@/services/github-releases';
import * as McuMgrModule from '@/services/mcumgr';
import {
    defaultSelectedDevice,
    mockBluetooth,
    mockClientMethods,
    mockGitHub,
    mockRelease,
    renderWithMcuMgr,
} from '@/test/firmware-mocks';

jest.mock('expo-router', () => {
    const actual = jest.requireActual('expo-router');
    return { ...actual, useRouter: () => ({ push: jest.fn(), back: jest.fn() }) };
});

describe('ExtensionManagementScreen', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
    jest.spyOn(console, 'warn').mockImplementation(() => {});
    jest.spyOn(console, 'error').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  const HASH_A = 'a'.repeat(64);
  const HASH_B = 'b'.repeat(64);

  /** mockRelease plus two bare .llext assets, as the real release workflow publishes. */
  const releaseWithExtensions: GitHubReleases.GitHubRelease = {
    ...mockRelease,
    assets: [
      ...mockRelease.assets,
      {
        id: 30,
        name: 'demo_wave.llext',
        browser_download_url: 'https://example.com/demo_wave.llext',
        size: 3216,
        content_type: 'application/octet-stream',
        digest: `sha256:${HASH_A}`,
      },
      {
        id: 31,
        name: 'plasma.llext',
        browser_download_url: 'https://example.com/plasma.llext',
        size: 5024,
        content_type: 'application/octet-stream',
        digest: `sha256:${HASH_A}`,
      },
    ],
  };

  /** Bytes the mocked download serves for plasma.llext. */
  const PLASMA_BYTES = Uint8Array.from([1, 2, 3]);

  /** releaseWithExtensions, but plasma's digest matches `bytes`. */
  function releaseServing(bytes: Uint8Array): GitHubReleases.GitHubRelease {
    return {
      ...releaseWithExtensions,
      assets: releaseWithExtensions.assets.map(a =>
        a.name === 'plasma.llext' ? { ...a, digest: `sha256:${sha256.hex(bytes)}` } : a
      ),
    };
  }

  function mockDownload(bytes: Uint8Array) {
    (global as any).fetch = jest.fn(async () => ({
      ok: true,
      status: 200,
      statusText: 'OK',
      arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
    }));
  }

  function mockExtensionClient(
    hashes: Record<string, string | null>,
    deviceFiles: McuMgrModule.DeviceFileEntry[] | 'unsupported' = []
  ) {
    const spies = mockClientMethods({
      listDeviceFiles:
        deviceFiles === 'unsupported'
          ? async () => {
              // Old firmware: the group answers with a bare MGMT_ERR_ENOTSUP rc.
              throw new McuMgrModule.SmpCommandError('File list error: rc=8', 8, undefined);
            }
          : async () => deviceFiles,
    });
    const getFileSha256 = jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'getFileSha256')
      .mockImplementation(async (path: string) => hashes[path] ?? null);
    const uploadFile = jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'uploadFile')
      .mockImplementation(async () => undefined);
    return { ...spies, getFileSha256, uploadFile };
  }

  /** Auto-accept the destructive confirm; returns the spy for assertions. */
  function autoConfirmRemove() {
    return jest.spyOn(Alert, 'alert').mockImplementation((_t, _m, buttons) => {
      const destructive = buttons?.find(b => b.style === 'destructive');
      destructive?.onPress?.();
    });
  }

  it('reports each released extension with its status and action', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    mockExtensionClient(
      {
        '/NAND:/ext/demo_wave.llext': HASH_A, // matches the release
        '/NAND:/ext/plasma.llext': HASH_B, // differs
      },
      [
        { name: 'demo_wave.llext', onDisk: true, loaded: true },
        { name: 'plasma.llext', onDisk: true, loaded: true },
      ]
    );

    const { findByText, findByTestId, queryByTestId } = renderWithMcuMgr(
      <ExtensionManagementScreen />
    );

    expect(await findByText('demo_wave.llext')).toBeTruthy();
    expect(await findByText('Installed · up to date')).toBeTruthy();
    expect(await findByText('plasma.llext')).toBeTruthy();
    expect(await findByText('Update available')).toBeTruthy();
    expect(await findByTestId('ext-mgmt-install-plasma.llext')).toBeTruthy();
    // Up to date offers no install action.
    expect(queryByTestId('ext-mgmt-install-demo_wave.llext')).toBeNull();
    expect(await findByText('Everything on your sunglasses comes from this release.')).toBeTruthy();
  });

  it('names and highlights device files the release does not ship', async () => {
    // The whole point of FILE_MGMT LIST: hello.llext is finally visible and
    // removable over BLE instead of a nameless count.
    mockBluetooth(defaultSelectedDevice);
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    mockExtensionClient(
      {
        '/NAND:/ext/demo_wave.llext': HASH_A,
        '/NAND:/ext/plasma.llext': HASH_A,
      },
      [
        { name: 'demo_wave.llext', onDisk: true, loaded: true },
        { name: 'plasma.llext', onDisk: true, loaded: true },
        { name: 'hello.llext', onDisk: true, loaded: true, displayName: 'Hello Extension' },
      ]
    );

    const { findByText, findByTestId } = renderWithMcuMgr(<ExtensionManagementScreen />);

    expect(await findByTestId('ext-mgmt-unmanaged-hello.llext')).toBeTruthy();
    expect(await findByText('hello.llext')).toBeTruthy();
    expect(await findByText('Hello Extension')).toBeTruthy();
    expect(await findByText('Not part of this release')).toBeTruthy();
    expect(await findByTestId('ext-mgmt-remove-hello.llext')).toBeTruthy();
  });

  it('renders the boot-scoped divergent states from LIST flags', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    mockExtensionClient(
      {
        '/NAND:/ext/demo_wave.llext': HASH_A,
        '/NAND:/ext/plasma.llext': HASH_A,
      },
      [
        { name: 'demo_wave.llext', onDisk: true, loaded: true },
        { name: 'plasma.llext', onDisk: true, loaded: true },
        // Uploaded since boot: on disk, no slot yet.
        { name: 'fresh.llext', onDisk: true, loaded: false },
        // Deleted since boot: slot ghost, persistent until restart.
        { name: 'gone.llext', onDisk: false, loaded: true, retired: true },
      ]
    );

    const { findByText, queryByTestId } = renderWithMcuMgr(<ExtensionManagementScreen />);

    expect(await findByText('Takes effect after restart')).toBeTruthy();
    expect(await findByText('Removed — restart to free the slot')).toBeTruthy();
    // A ghost has no file to delete, so no Remove button.
    await waitFor(() => expect(queryByTestId('ext-mgmt-remove-gone.llext')).toBeNull());
  });

  it('removes a file after the destructive confirm and offers a restart', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    const spies = mockExtensionClient(
      {
        '/NAND:/ext/demo_wave.llext': HASH_A,
        '/NAND:/ext/plasma.llext': HASH_A,
      },
      [
        { name: 'demo_wave.llext', onDisk: true, loaded: true },
        { name: 'plasma.llext', onDisk: true, loaded: true },
        { name: 'hello.llext', onDisk: true, loaded: true },
      ]
    );
    const alert = autoConfirmRemove();

    const { findByTestId } = renderWithMcuMgr(<ExtensionManagementScreen />);
    fireEvent.press(await findByTestId('ext-mgmt-remove-hello.llext'));

    await waitFor(() => expect(spies.deleteDeviceFile).toHaveBeenCalledWith('hello.llext', 'ext'));
    expect(alert).toHaveBeenCalled();
    // Extension changes are boot-scoped, so a successful mutation surfaces the
    // restart offer, driven by the OS-group reset.
    const restart = await findByTestId('ext-mgmt-restart');
    fireEvent.press(restart);
    await waitFor(() => expect(spies.reset).toHaveBeenCalled());
  });

  it('does not delete when the confirm is cancelled', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    const spies = mockExtensionClient(
      {
        '/NAND:/ext/demo_wave.llext': HASH_A,
        '/NAND:/ext/plasma.llext': HASH_A,
      },
      [{ name: 'hello.llext', onDisk: true, loaded: true }]
    );
    jest.spyOn(Alert, 'alert').mockImplementation((_t, _m, buttons) => {
      buttons?.find(b => b.style === 'cancel')?.onPress?.();
    });

    const { findByTestId, queryByTestId } = renderWithMcuMgr(<ExtensionManagementScreen />);
    fireEvent.press(await findByTestId('ext-mgmt-remove-hello.llext'));

    await waitFor(() => expect(spies.deleteDeviceFile).not.toHaveBeenCalled());
    expect(queryByTestId('ext-mgmt-restart')).toBeNull();
  });

  it('installs a single extension per row, never in bulk', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseServing(PLASMA_BYTES) });
    const spies = mockExtensionClient(
      {
        '/NAND:/ext/demo_wave.llext': HASH_B, // also outdated — must NOT be uploaded
        '/NAND:/ext/plasma.llext': null, // absent
      },
      [{ name: 'demo_wave.llext', onDisk: true, loaded: true }]
    );
    mockDownload(PLASMA_BYTES);

    const { findByTestId } = renderWithMcuMgr(<ExtensionManagementScreen />);
    fireEvent.press(await findByTestId('ext-mgmt-install-plasma.llext'));

    await waitFor(() => expect(spies.uploadFile).toHaveBeenCalledTimes(1));
    expect(spies.uploadFile.mock.calls[0][0]).toBe('/NAND:/ext/plasma.llext');
  });

  it('hides list/remove but keeps per-row install on firmware without the group', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    mockExtensionClient(
      {
        '/NAND:/ext/demo_wave.llext': HASH_A,
        '/NAND:/ext/plasma.llext': null,
      },
      'unsupported'
    );

    const { findByText, findByTestId, queryByTestId } = renderWithMcuMgr(
      <ExtensionManagementScreen />
    );

    // The release section still works: install is plain fs_mgmt upload.
    expect(await findByTestId('ext-mgmt-install-plasma.llext')).toBeTruthy();
    // No unmanaged section, no remove buttons, and an explanation instead.
    expect(queryByTestId('ext-mgmt-unmanaged')).toBeNull();
    expect(queryByTestId('ext-mgmt-remove-demo_wave.llext')).toBeNull();
    expect(
      await findByText(/cannot list or remove extension files over Bluetooth/)
    ).toBeTruthy();
  });

  it('does not re-issue SMP traffic when the context yields fresh identities', async () => {
    // The regression app/CLAUDE.md mandates: an effect that issues BLE traffic and
    // writes the result into state must not be re-armed by its own writes. Uses
    // mockImplementation so every render returns a NEW selectedDevice object.
    const setSelectedDevice = jest.fn();
    jest.spyOn(BluetoothContext, 'useBluetooth').mockImplementation(
      () =>
        ({
          selectedDevice: { ...defaultSelectedDevice },
          setSelectedDevice,
        }) as any
    );
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    const spies = mockExtensionClient(
      {
        '/NAND:/ext/demo_wave.llext': HASH_A,
        '/NAND:/ext/plasma.llext': HASH_A,
      },
      []
    );

    const { findAllByText, rerender } = renderWithMcuMgr(<ExtensionManagementScreen />);
    await findAllByText('Installed · up to date');

    // The check legitimately runs once before the release lookup lands (empty
    // assets) and once after — what must NOT happen is any growth from renders.
    const hashCalls = spies.getFileSha256.mock.calls.length;
    const listCalls = spies.listDeviceFiles.mock.calls.length;
    expect(hashCalls).toBe(2); // one per released extension, once

    // Re-render the SAME tree several times with fresh context identities.
    for (let i = 0; i < 3; i++) {
      rerender(<ExtensionManagementScreen />);
    }
    await waitFor(() => expect(spies.getFileSha256).toHaveBeenCalledTimes(hashCalls));
    expect(spies.listDeviceFiles).toHaveBeenCalledTimes(listCalls);
  });
});
