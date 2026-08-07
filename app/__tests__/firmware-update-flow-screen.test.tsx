import { render, waitFor } from '@testing-library/react-native';
import React from 'react';

import FirmwareUpdateFlow from '@/app/firmware-update/flow';
import * as BluetoothContext from '@/context/bluetooth-context';
import * as FirmwareSource from '@/services/firmware-source';
import { mockClientMethods } from '@/test/firmware-mocks';

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
