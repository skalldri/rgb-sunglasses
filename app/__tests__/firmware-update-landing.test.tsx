import { fireEvent, waitFor } from '@testing-library/react-native';
import React from 'react';

import FirmwareUpdateLanding from '@/app/firmware-update/index';
import * as GitHubReleases from '@/services/github-releases';
import * as DocumentPicker from 'expo-document-picker';
import {
    defaultSelectedDevice,
    mockBluetooth,
    mockClientMethods,
    mockGitHub,
    renderWithMcuMgr,
} from '@/test/firmware-mocks';

const mockPush = jest.fn();
jest.mock('expo-router', () => {
    const actual = jest.requireActual('expo-router');
    return {
        ...actual,
        useRouter: () => ({ push: mockPush, back: jest.fn() }),
    };
});

describe('FirmwareUpdateLanding', () => {
    beforeEach(() => {
        jest.spyOn(console, 'log').mockImplementation(() => {});
        jest.spyOn(console, 'warn').mockImplementation(() => {});
        jest.spyOn(console, 'error').mockImplementation(() => {});
        mockPush.mockClear();
    });

    afterEach(() => {
        jest.restoreAllMocks();
    });

    it('offers the latest release front and centre when one is newer', async () => {
        mockBluetooth(defaultSelectedDevice);
        mockClientMethods({
            getImageState: async () => ({
                images: [{ image: 0, slot: 0, version: '1.0.0', active: true }],
            }),
        });
        mockGitHub();

        const { findByText } = renderWithMcuMgr(<FirmwareUpdateLanding />);

        expect(await findByText('Update Available')).toBeTruthy();
        expect(await findByText('v2.0.0')).toBeTruthy();
        expect(await findByText('Install Update')).toBeTruthy();
    });

    it('says up to date when the device already has the latest', async () => {
        mockBluetooth(defaultSelectedDevice);
        mockClientMethods({
            getImageState: async () => ({
                images: [{ image: 0, slot: 0, version: '2.0.0', active: true }],
            }),
        });
        mockGitHub();

        const { findByText } = renderWithMcuMgr(<FirmwareUpdateLanding />);
        expect(await findByText('Up to date (v2.0.0)')).toBeTruthy();
    });

    it('hands the release URL to the flow rather than downloading here', async () => {
        mockBluetooth(defaultSelectedDevice);
        mockClientMethods({
            getImageState: async () => ({
                images: [{ image: 0, slot: 0, version: '1.0.0', active: true }],
            }),
        });
        mockGitHub();

        const { findByText } = renderWithMcuMgr(<FirmwareUpdateLanding />);
        fireEvent.press(await findByText('Install Update'));

        expect(mockPush).toHaveBeenCalledWith({
            pathname: '/firmware-update/flow',
            params: expect.objectContaining({
                source: 'release',
                url: 'https://example.com/firmware_proto0_v2.0.0.zip',
            }),
        });
    });

    it('hands a picked file to the flow as a uri', async () => {
        mockBluetooth(defaultSelectedDevice);
        mockClientMethods();
        mockGitHub();
        (DocumentPicker.getDocumentAsync as jest.Mock).mockResolvedValue({
            canceled: false,
            assets: [{ uri: 'file:///cache/fw.zip', name: 'fw.zip' }],
        });

        const { findByText } = renderWithMcuMgr(<FirmwareUpdateLanding />);
        fireEvent.press(await findByText('Install from a .zip file'));

        await waitFor(() =>
            expect(mockPush).toHaveBeenCalledWith({
                pathname: '/firmware-update/flow',
                params: { source: 'file', uri: 'file:///cache/fw.zip', name: 'fw.zip' },
            })
        );
    });

    it('exposes stable testIDs for hardware validation runs', async () => {
        // See the matching test on the flow screen: these are the handles /drive-app
        // taps by, and fiber-based fallbacks press covered screens.
        mockBluetooth(defaultSelectedDevice);
        mockClientMethods({
            getImageState: async () => ({
                images: [{ image: 0, slot: 0, version: '1.0.0', active: true }],
            }),
        });
        mockGitHub();

        const { findByTestId, getByTestId } = renderWithMcuMgr(<FirmwareUpdateLanding />);

        expect(await findByTestId('fw-update-install-release')).toBeTruthy();
        expect(getByTestId('fw-update-pick-zip')).toBeTruthy();
        expect(getByTestId('fw-update-landing-sync-extensions')).toBeTruthy();
        expect(getByTestId('fw-update-landing-debug')).toBeTruthy();
        expect(getByTestId('fw-update-landing-back')).toBeTruthy();
    });

    it('shows a disconnected device as neutral status, never as a red error', async () => {
        // The whole point of the redesign: "No device connected" is a state, not a fault.
        mockBluetooth(null);
        mockClientMethods();

        const { findByText, queryByText } = renderWithMcuMgr(<FirmwareUpdateLanding />);

        expect(
            await findByText(/No device connected. Connect to your sunglasses/)
        ).toBeTruthy();
        expect(queryByText('Update Available')).toBeNull();
    });

    it('says it is reconnecting rather than disconnected while the loop runs', async () => {
        jest.spyOn(require('@/context/bluetooth-context'), 'useBluetooth').mockReturnValue({
            selectedDevice: null,
            setSelectedDevice: jest.fn(),
            reconnectingDevice: { mac: 'AA:BB:CC', name: 'RGB Sunglasses' },
        } as any);
        mockClientMethods();

        const { findByText } = renderWithMcuMgr(<FirmwareUpdateLanding />);
        expect(await findByText('Reconnecting to RGB Sunglasses…')).toBeTruthy();
    });

    it('explains a client-init failure instead of spinning on "Checking your device"', async () => {
        // Connected device, but SMP init threw (e.g. firmware with no SMP
        // characteristic). Board detection cannot run, so both of its outputs stay
        // empty and the spinner branch was permanently true.
        mockBluetooth(defaultSelectedDevice);
        mockClientMethods({
            initialize: async () => {
                throw new Error('SMP characteristic not found');
            },
        });
        mockGitHub();

        const { findByText, queryByText } = renderWithMcuMgr(<FirmwareUpdateLanding />);

        expect(await findByText(/Failed to initialize: SMP characteristic not found/)).toBeTruthy();
        expect(queryByText('Checking your device…')).toBeNull();
    });

    it('surfaces a GitHub failure without blocking the other options', async () => {
        mockBluetooth(defaultSelectedDevice);
        mockClientMethods();
        jest.spyOn(GitHubReleases, 'fetchLatestFirmwareRelease').mockRejectedValue(
            new Error('network down')
        );

        const { findByText } = renderWithMcuMgr(<FirmwareUpdateLanding />);

        expect(await findByText('Update check failed: network down')).toBeTruthy();
        expect(await findByText('Install from a .zip file')).toBeTruthy();
        expect(await findByText('FW Update Debug')).toBeTruthy();
    });
});
