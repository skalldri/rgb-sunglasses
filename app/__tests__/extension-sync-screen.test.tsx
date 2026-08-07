import { fireEvent, waitFor } from '@testing-library/react-native';
import React from 'react';
import * as BluetoothContext from '@/context/bluetooth-context';
import { sha256 } from 'js-sha256';

import ExtensionSyncScreen from '@/app/firmware-update/extensions';
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

describe('ExtensionSyncScreen', () => {
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
        name: 'hello.llext',
        browser_download_url: 'https://example.com/hello.llext',
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

  function mockExtensionClient(hashes: Record<string, string | null>) {
    const getFileSha256 = jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'getFileSha256')
      .mockImplementation(async (path: string) => hashes[path] ?? null);
    const uploadFile = jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'uploadFile')
      .mockImplementation(async () => undefined);
    return { getFileSha256, uploadFile };
  }

  it('reports each extension as up to date, outdated or not installed', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods();
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    mockExtensionClient({
      '/NAND:/ext/hello.llext': HASH_A, // matches the release
      '/NAND:/ext/plasma.llext': HASH_B, // differs
    });

    const { findByText } = renderWithMcuMgr(<ExtensionSyncScreen />);

    expect(await findByText('Extensions')).toBeTruthy();
    expect(await findByText('hello.llext')).toBeTruthy();
    expect(await findByText('Up to date')).toBeTruthy();
    expect(await findByText('plasma.llext')).toBeTruthy();
    expect(await findByText('Update available')).toBeTruthy();
  });

  it('offers to install an extension the device does not have', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods();
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    mockExtensionClient({ '/NAND:/ext/hello.llext': HASH_A }); // plasma absent

    const { findByText } = renderWithMcuMgr(<ExtensionSyncScreen />);

    expect(await findByText('Not installed')).toBeTruthy();
    expect(await findByText('Sync Extensions')).toBeTruthy();
  });

  it('uploads only the extensions that differ, to their device paths', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods();
    // The release must advertise the digest of the bytes the download actually
    // returns, or the integrity check below refuses to upload them.
    mockGitHub({
      fetchLatestFirmwareRelease: async () => releaseServing(PLASMA_BYTES),
    });
    const { uploadFile } = mockExtensionClient({
      '/NAND:/ext/hello.llext': HASH_A,
      '/NAND:/ext/plasma.llext': HASH_B,
    });
    mockDownload(PLASMA_BYTES);

    const { findByText } = renderWithMcuMgr(<ExtensionSyncScreen />);
    fireEvent.press(await findByText('Sync Extensions'));

    await waitFor(() => expect(uploadFile).toHaveBeenCalledTimes(1));
    expect(uploadFile.mock.calls[0][0]).toBe('/NAND:/ext/plasma.llext');
  });

  it('refuses to upload a download that does not match the release digest', async () => {
    // A truncated CDN response would otherwise overwrite a working extension
    // with corrupt bytes that only fail at the next boot.
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods();
    mockGitHub({
      fetchLatestFirmwareRelease: async () => releaseServing(PLASMA_BYTES),
    });
    const { uploadFile } = mockExtensionClient({
      '/NAND:/ext/hello.llext': HASH_A,
      '/NAND:/ext/plasma.llext': HASH_B,
    });
    mockDownload(Uint8Array.from([9, 9, 9])); // not what the digest promises

    const { findByText } = renderWithMcuMgr(<ExtensionSyncScreen />);
    fireEvent.press(await findByText('Sync Extensions'));

    // Nothing is written to the device, and the failure is surfaced with a
    // retry. (The exact message is asserted in the extension-sync unit tests.)
    expect(await findByText(/Extension sync failed/)).toBeTruthy();
    expect(await findByText('Retry Sync')).toBeTruthy();
    expect(uploadFile).not.toHaveBeenCalled();
  });

  it('keeps the entry list and a retry button when a sync fails part-way', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods();
    mockGitHub({
      fetchLatestFirmwareRelease: async () => releaseServing(PLASMA_BYTES),
    });
    mockExtensionClient({
      '/NAND:/ext/hello.llext': HASH_A,
      '/NAND:/ext/plasma.llext': HASH_B,
    });
    jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'uploadFile')
      .mockImplementation(async () => {
        throw new Error('SMP request timeout after 5000ms');
      });
    mockDownload(PLASMA_BYTES);

    const { findByText } = renderWithMcuMgr(<ExtensionSyncScreen />);
    fireEvent.press(await findByText('Sync Extensions'));

    expect(await findByText(/Extension sync failed/)).toBeTruthy();
    expect(await findByText('plasma.llext')).toBeTruthy();
    expect(await findByText('Retry Sync')).toBeTruthy();
  });

  it('treats an unhashable file as needing repair instead of failing the whole check', async () => {
    // A sync interrupted by a BLE drop leaves a zero-length file, which
    // fs_mgmt answers with FILE_EMPTY (16) - not FILE_NOT_FOUND. Without
    // per-entry handling, that one file made every extension unsyncable.
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods();
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'getFileSha256')
      .mockImplementation(async (path: string) => {
        if (path.includes('plasma')) {
          throw new McuMgrModule.SmpCommandError(
            'File hash error: group=8, rc=16',
            McuMgrModule.FsMgmtError.FILE_EMPTY,
            McuMgrModule.SmpGroup.FS
          );
        }
        return HASH_A;
      });

    const { findByText } = renderWithMcuMgr(<ExtensionSyncScreen />);

    expect(await findByText('Needs repair')).toBeTruthy();
    // The healthy one is still listed and the card still offers a sync.
    expect(await findByText('Up to date')).toBeTruthy();
    expect(await findByText('Sync Extensions')).toBeTruthy();
  });

  it('still fails the whole check for an error that is not file-specific', async () => {
    // Firmware with no file-management group answers with a group-less rc;
    // reporting "needs upload" there would start an upload that cannot work.
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods();
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'getFileSha256')
      .mockImplementation(async () => {
        throw new McuMgrModule.SmpCommandError('File hash error: rc=8', 8, undefined);
      });

    const { findByText } = renderWithMcuMgr(<ExtensionSyncScreen />);

    expect(await findByText(/Extension check unavailable/)).toBeTruthy();
  });

  it('says nothing needs doing when every extension matches', async () => {
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods();
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    mockExtensionClient({
      '/NAND:/ext/hello.llext': HASH_A,
      '/NAND:/ext/plasma.llext': HASH_A,
    });

    const { findByText, queryByText } = renderWithMcuMgr(<ExtensionSyncScreen />);

    expect(await findByText('All extensions match this release.')).toBeTruthy();
    expect(queryByText('Sync Extensions')).toBeNull();
  });

  it('surfaces a firmware without file management instead of failing the modal', async () => {
    // Firmware predating CONFIG_MCUMGR_GRP_FS answers every FS command with an
    // error; that must not look like a broken update.
    mockBluetooth(defaultSelectedDevice);
    mockClientMethods();
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    jest
      .spyOn(McuMgrModule.McuMgrClient.prototype, 'getFileSha256')
      .mockImplementation(async () => {
        throw new Error('File hash error: rc=8');
      });

    const { findByText } = renderWithMcuMgr(<ExtensionSyncScreen />);

    expect(
      await findByText('Extension check unavailable: File hash error: rc=8')
    ).toBeTruthy();
  });

  it('reports extensions on the device that the release does not ship', async () => {
    // Device exposes three extension animation services but the release ships
    // two, so one is unmanaged. Counted, not named: the device reports manifest
    // display names while the release ships file names.
    mockBluetooth({
      ...defaultSelectedDevice,
      characteristicsByService: {
        '12345678-1234-5678-4000-56789abd0000': {},
        '12345678-1234-5678-4100-56789abd0000': {},
        '12345678-1234-5678-4200-56789abd0000': {},
        '12345678-1234-5678-0500-56789abd0000': {}, // built-in, must not count
      },
    });
    mockClientMethods();
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    mockExtensionClient({
      '/NAND:/ext/hello.llext': HASH_A,
      '/NAND:/ext/plasma.llext': HASH_A,
    });

    const { findByText } = renderWithMcuMgr(<ExtensionSyncScreen />);

    expect(
      await findByText(/1 extension on this device is not part of this release\./)
    ).toBeTruthy();
  });

  it('does not re-hash extensions when the context yields fresh identities', async () => {
    // The regression app/CLAUDE.md mandates: an effect that issues BLE traffic and
    // writes the result into state must not be re-armed by its own writes. Uses
    // mockImplementation so every render returns a NEW selectedDevice object.
    const setSelectedDevice = jest.fn();
    jest.spyOn(BluetoothContext, 'useBluetooth').mockImplementation(
      () =>
        ({
          selectedDevice: { ...defaultSelectedDevice, characteristicsByService: {} },
          setSelectedDevice,
        }) as any
    );
    mockClientMethods();
    mockGitHub({ fetchLatestFirmwareRelease: async () => releaseWithExtensions });
    const { getFileSha256 } = mockExtensionClient({
      '/NAND:/ext/hello.llext': HASH_A,
      '/NAND:/ext/plasma.llext': HASH_A,
    });

    const { findByText, rerender } = renderWithMcuMgr(<ExtensionSyncScreen />);
    await findByText('All extensions match this release.');

    const callsAfterFirstCheck = getFileSha256.mock.calls.length;
    expect(callsAfterFirstCheck).toBe(2); // one per released extension

    // Re-render the SAME tree several times with fresh context identities.
    for (let i = 0; i < 3; i++) {
      rerender(<ExtensionSyncScreen />);
    }
    await waitFor(() => expect(getFileSha256).toHaveBeenCalledTimes(callsAfterFirstCheck));
  });
});
