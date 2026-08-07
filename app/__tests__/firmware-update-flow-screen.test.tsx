import { fireEvent, render, waitFor } from '@testing-library/react-native';
import React from 'react';

import FirmwareUpdateFlow from '@/app/firmware-update/flow';
import * as BluetoothContext from '@/context/bluetooth-context';
import * as FirmwareSource from '@/services/firmware-source';
import * as GitHubReleases from '@/services/github-releases';
import * as McuMgrModule from '@/services/mcumgr';
import { mockClientMethods, mockRelease, renderWithMcuMgr } from '@/test/firmware-mocks';
import fixture from './fixtures/mcuboot-image-header.json';

const APP_IMAGE = Uint8Array.from(fixture.bytes as number[]);
const APP_TLV_BYTES = Uint8Array.from(
    ((fixture.expectedSha256 as string).match(/../g) as string[]).map(h => parseInt(h, 16))
);

let mockParams: Record<string, string> = {};
jest.mock('expo-router', () => {
    const actual = jest.requireActual('expo-router');
    return {
        ...actual,
        useRouter: () => ({ push: jest.fn(), back: jest.fn() }),
        useLocalSearchParams: () => mockParams,
    };
});

/**
 * Screen-level coverage for the source-resolution path.
 *
 * The old modal's "loads firmware package via parser" / "shows package parsing error"
 * tests covered picking a zip end to end. After the split, the landing page only hands
 * over a URI and the flow hook receives an already-parsed package — this screen is
 * where parsing actually happens, so it needs its own tests or the path ships untested.
 */
describe('FirmwareUpdateFlow screen', () => {
    beforeEach(() => {
        mockParams = {};
        jest.spyOn(console, 'log').mockImplementation(() => {});
        jest.spyOn(console, 'warn').mockImplementation(() => {});
        jest.spyOn(console, 'error').mockImplementation(() => {});
        jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
            selectedDevice: { name: 'RGB Sunglasses', mac: 'AA:BB:CC', device: {} },
            setSelectedDevice: jest.fn(),
            reconnectingDevice: null,
        } as any);
        mockClientMethods();
    });

    afterEach(() => jest.restoreAllMocks());

    it('parses a picked .zip and offers to install it', async () => {
        mockParams = { source: 'file', uri: 'file:///cache/fw.zip', name: 'fw.zip' };
        jest.spyOn(FirmwareSource, 'loadPackage').mockResolvedValue({
            manifest: { 'format-version': 1, time: 0, name: 'fw', files: [] },
            images: [
                {
                    manifest: { file: 'fw.signed.bin', image_index: '0', size: 4 } as any,
                    data: Uint8Array.from([1, 2, 3, 4]),
                    parsedHeader: { magic: 0, version: '2.1.0+0', imageSize: 4 },
                },
            ],
        } as any);

        const { findByText } = render(<FirmwareUpdateFlow />);

        expect(await findByText('Ready to install')).toBeTruthy();
        expect(await findByText('Image 0: fw.signed.bin')).toBeTruthy();
        expect(await findByText('Install')).toBeTruthy();
    });

    it('surfaces a parse failure instead of spinning on "Preparing"', async () => {
        mockParams = { source: 'file', uri: 'file:///cache/broken.zip', name: 'broken.zip' };
        jest.spyOn(FirmwareSource, 'loadPackage').mockRejectedValue(
            new Error('No manifest.json found in firmware package')
        );

        const { findByText } = render(<FirmwareUpdateFlow />);

        expect(await findByText('No manifest.json found in firmware package')).toBeTruthy();
    });

    it('surfaces a download failure for the release path', async () => {
        mockParams = { source: 'release', url: 'https://example.com/fw.zip', version: '2.1.0' };
        jest.spyOn(FirmwareSource, 'loadPackage').mockRejectedValue(
            new Error('Download was cancelled')
        );

        const { findByText } = render(<FirmwareUpdateFlow />);
        expect(await findByText('Download was cancelled')).toBeTruthy();
    });

    it('explains itself when the screen is reached without any source params', async () => {
        // Reachable via Android restoring the nested navigation state, a deep link, or
        // dev navigation. Previously left a permanent "Preparing update" spinner that
        // was indistinguishable from a slow download.
        mockParams = {};
        const loadPackage = jest.spyOn(FirmwareSource, 'loadPackage');

        const { findByText } = render(<FirmwareUpdateFlow />);

        expect(await findByText(/No update source was provided/)).toBeTruthy();
        expect(loadPackage).not.toHaveBeenCalled();
    });

    it('rejects an unrecognised source kind', async () => {
        mockParams = { source: 'carrier-pigeon' };
        const { findByText } = render(<FirmwareUpdateFlow />);
        expect(await findByText(/Unknown update source: carrier-pigeon/)).toBeTruthy();
    });

    it('lists the extensions on the restart step instead of only counting them', async () => {
        // The staged step embeds the real ExtensionSyncCard, so the user can see which
        // files are involved and watch the upload progress - a bare "1 extension will be
        // updated" line gave no way to tell what was about to change.
        mockParams = { source: 'file', uri: 'file:///cache/fw.zip', name: 'fw.zip' };
        jest.spyOn(FirmwareSource, 'loadPackage').mockResolvedValue({
            manifest: { 'format-version': 1, time: 0, name: 'fw', files: [] },
            images: [
                {
                    manifest: { file: 'fw.signed.bin', image_index: '0', size: 4 } as any,
                    data: APP_IMAGE,
                    parsedHeader: { magic: 0, version: '2.1.0+0', imageSize: 4 },
                },
            ],
        } as any);
        jest.spyOn(McuMgrModule.McuMgrClient.prototype, 'getImageState')
            .mockResolvedValueOnce({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_TLV_BYTES }],
            } as any)
            .mockResolvedValue({
                images: [
                    { image: 0, slot: 1, version: '2.1.0', hash: APP_TLV_BYTES, pending: true },
                ],
            } as any);
        // One extension out of date, one current.
        jest.spyOn(GitHubReleases, 'fetchLatestFirmwareRelease').mockResolvedValue({
            ...mockRelease,
            assets: [
                ...mockRelease.assets,
                {
                    id: 30,
                    name: 'plasma.llext',
                    browser_download_url: 'https://example.com/plasma.llext',
                    size: 10,
                    content_type: 'application/octet-stream',
                    digest: `sha256:${'a'.repeat(64)}`,
                },
            ],
        } as any);
        jest.spyOn(McuMgrModule.McuMgrClient.prototype, 'getFileSha256').mockResolvedValue(
            'b'.repeat(64)
        );

        const { findByText } = renderWithMcuMgr(<FirmwareUpdateFlow />);
        fireEvent.press(await findByText('Install'));

        // Named, with its status - not just a count.
        expect(await findByText('plasma.llext')).toBeTruthy();
        expect(await findByText('Update available')).toBeTruthy();
        // The card must not offer a competing Sync button here; the restart drives it.
        expect(await findByText('Restart and Install')).toBeTruthy();
    });

    it('exposes stable testIDs for hardware validation runs', async () => {
        // /drive-app taps these by testID. Without them, execbro falls back to fiber
        // matching, which searches *covered* screens (a pushed Expo Router screen does
        // not unmount its parent) and silently presses a button the user cannot see.
        // These IDs are an API for the validation runs - deleting one breaks a run in a
        // way that looks like a hardware fault, so pin them here.
        mockParams = { source: 'file', uri: 'file:///cache/fw.zip', name: 'fw.zip' };
        jest.spyOn(FirmwareSource, 'loadPackage').mockResolvedValue({
            manifest: { 'format-version': 1, time: 0, name: 'fw', files: [] },
            images: [
                {
                    manifest: { file: 'fw.signed.bin', image_index: '0', size: 4 } as any,
                    data: Uint8Array.from([1, 2, 3, 4]),
                    parsedHeader: { magic: 0, version: '2.1.0+0', imageSize: 4 },
                },
            ],
        } as any);

        const { findByTestId, getByTestId } = render(<FirmwareUpdateFlow />);

        expect(await findByTestId('fw-update-install')).toBeTruthy();
        // The step container carries the current step, so a run can read state rather
        // than infer it from the title text.
        expect(getByTestId('fw-update-step-ready')).toBeTruthy();
        expect(getByTestId('fw-update-image-0')).toBeTruthy();
        expect(getByTestId('fw-update-image-0-status')).toBeTruthy();
        expect(getByTestId('fw-update-flow-back')).toBeTruthy();
    });

    it('does not re-resolve the source on re-render', async () => {
        // The effect downloads; re-running it would re-download an ~850 KB asset.
        mockParams = { source: 'release', url: 'https://example.com/fw.zip', version: '2.1.0' };
        const loadPackage = jest.spyOn(FirmwareSource, 'loadPackage').mockResolvedValue({
            manifest: { 'format-version': 1, time: 0, name: 'fw', files: [] },
            images: [],
        } as any);

        const { rerender } = render(<FirmwareUpdateFlow />);
        await waitFor(() => expect(loadPackage).toHaveBeenCalledTimes(1));

        for (let i = 0; i < 3; i++) rerender(<FirmwareUpdateFlow />);
        await waitFor(() => expect(loadPackage).toHaveBeenCalledTimes(1));
    });
});
