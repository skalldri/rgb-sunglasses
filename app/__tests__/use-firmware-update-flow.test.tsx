import { act, renderHook, waitFor } from '@testing-library/react-native';
import React from 'react';

import * as BluetoothContext from '@/context/bluetooth-context';
import * as McuMgrClientContext from '@/context/mcumgr-client-context';
import { useFirmwareUpdateFlow } from '@/hooks/use-firmware-update-flow';
import { FirmwarePackage } from '@/services/firmware-package';

/** A two-image package, matching what the real dfu_application.zip carries. */
function makePackage(): FirmwarePackage {
    return {
        manifest: { 'format-version': 0, time: 0, name: 'fw', files: [] as any },
        images: [
            {
                manifest: {
                    file: 'fw.signed.bin',
                    image_index: '0',
                    size: 4,
                    type: 'application',
                    board: 'proto0',
                } as any,
                data: Uint8Array.from([1, 2, 3, 4]),
                parsedHeader: { magic: 0, version: '2.1.0+0', imageSize: 4 },
            },
        ],
    } as FirmwarePackage;
}

const APP_HASH = Uint8Array.from([0xaa, 0xbb]);
const OTHER_HASH = Uint8Array.from([0x99, 0x88]);

interface Harness {
    client: any;
    device: any;
}

function mockEnv({ client, device }: Harness) {
    jest.spyOn(McuMgrClientContext, 'useMcuMgrClientContext').mockReturnValue({
        client,
        isInitializing: false,
        error: client ? '' : 'No device connected',
    } as any);
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
        selectedDevice: device,
        setSelectedDevice: jest.fn(),
        reconnectingDevice: null,
    } as any);
}

/** A client whose staged slot-1 image reports `stagedHash`, and whose post-reboot
 *  active slot-0 image reports `activeHash`. */
function makeClient(stagedHash: Uint8Array, activeHash: Uint8Array) {
    let rebooted = false;
    return {
        uploadImage: jest.fn(async (_d: Uint8Array, _i: number, onProgress?: any) => {
            onProgress?.(4, 4);
        }),
        getImageState: jest.fn(async () => {
            if (!rebooted) {
                return {
                    images: [
                        { image: 0, slot: 0, version: '1.0.0', active: true },
                        {
                            image: 0,
                            slot: 1,
                            version: '2.1.0',
                            hash: stagedHash,
                            pending: false,
                        },
                    ],
                };
            }
            return {
                images: [
                    { image: 0, slot: 0, version: '2.1.0', hash: activeHash, active: true },
                ],
            };
        }),
        setImageState: jest.fn(async () => {
            // After marking, the device reports the slot as pending.
            const inner = { images: [] };
            return inner;
        }),
        reset: jest.fn(async () => {
            rebooted = true;
        }),
        confirmCurrentImage: jest.fn(async () => undefined),
        markRebooted: () => {
            rebooted = true;
        },
    };
}

describe('useFirmwareUpdateFlow', () => {
    beforeEach(() => {
        jest.spyOn(console, 'log').mockImplementation(() => {});
    });
    afterEach(() => jest.restoreAllMocks());

    it('stages images for TEST, never as permanent', async () => {
        // The old screen called setImageState(hash, true) at upload time, leaving no way
        // back from a bad image. confirm=false is what enables MCUboot's revert.
        const client = makeClient(APP_HASH, APP_HASH);
        client.getImageState = jest
            .fn()
            .mockResolvedValueOnce({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH }],
            })
            .mockResolvedValue({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH, pending: true }],
            });
        mockEnv({ client, device: { mac: 'AA:BB:CC' } });

        const pkg = makePackage();
        const { result } = renderHook(() => useFirmwareUpdateFlow(pkg));
        await act(async () => {
            await result.current.startUpload();
        });

        expect(client.setImageState).toHaveBeenCalledWith(APP_HASH, false);
        expect(client.confirmCurrentImage).not.toHaveBeenCalled();
        await waitFor(() => expect(result.current.step).toBe('staged'));
    });

    it('fails staging when the device does not report the image pending', async () => {
        const client = makeClient(APP_HASH, APP_HASH);
        client.getImageState = jest.fn().mockResolvedValue({
            images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH, pending: false }],
        });
        mockEnv({ client, device: { mac: 'AA:BB:CC' } });

        const pkg = makePackage();
        const { result } = renderHook(() => useFirmwareUpdateFlow(pkg));
        await act(async () => {
            await result.current.startUpload();
        });

        expect(result.current.step).toBe('failed');
        expect(result.current.error).toMatch(/did not mark the uploaded image for test/);
    });

    it('treats the device disappearing after a reboot as progress, not an error', async () => {
        const client = makeClient(APP_HASH, APP_HASH);
        client.getImageState = jest
            .fn()
            .mockResolvedValueOnce({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH }],
            })
            .mockResolvedValue({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH, pending: true }],
            });
        mockEnv({ client, device: { mac: 'AA:BB:CC' } });

        const pkg = makePackage();
        const { result, rerender } = renderHook(() => useFirmwareUpdateFlow(pkg));
        await act(async () => {
            await result.current.startUpload();
        });
        await act(async () => {
            await result.current.reboot();
        });

        // The link drops — exactly what the old screen rendered as red "No device connected".
        mockEnv({ client, device: null });
        rerender({});

        await waitFor(() => expect(result.current.step).toBe('reconnecting'));
        expect(result.current.error).toBe('');
    });

    it('verifies against the hash captured at staging and then confirms', async () => {
        const client = makeClient(APP_HASH, APP_HASH);
        client.getImageState = jest
            .fn()
            .mockResolvedValueOnce({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH }],
            })
            .mockResolvedValueOnce({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH, pending: true }],
            })
            .mockResolvedValue({
                images: [{ image: 0, slot: 0, version: '2.1.0', hash: APP_HASH, active: true }],
            });
        mockEnv({ client, device: { mac: 'AA:BB:CC' } });

        const pkg = makePackage();
        const { result, rerender } = renderHook(() => useFirmwareUpdateFlow(pkg));
        await act(async () => {
            await result.current.startUpload();
        });
        await act(async () => {
            await result.current.reboot();
        });

        mockEnv({ client, device: null });
        rerender({});
        await waitFor(() => expect(result.current.step).toBe('reconnecting'));

        mockEnv({ client, device: { mac: 'AA:BB:CC' } });
        rerender({});

        await waitFor(() => expect(result.current.step).toBe('success'));
        expect(client.confirmCurrentImage).toHaveBeenCalledTimes(1);
        expect(result.current.images[0].verified).toBe(true);
    });

    it('does NOT confirm when the running image is not the one that was staged', async () => {
        // The safety property: an unconfirmed image is reverted by the bootloader, so
        // confirming on a mismatch would strand the device on unverified firmware.
        const client = makeClient(APP_HASH, OTHER_HASH);
        client.getImageState = jest
            .fn()
            .mockResolvedValueOnce({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH }],
            })
            .mockResolvedValueOnce({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH, pending: true }],
            })
            .mockResolvedValue({
                images: [{ image: 0, slot: 0, version: '1.0.0', hash: OTHER_HASH, active: true }],
            });
        mockEnv({ client, device: { mac: 'AA:BB:CC' } });

        const pkg = makePackage();
        const { result, rerender } = renderHook(() => useFirmwareUpdateFlow(pkg));
        await act(async () => {
            await result.current.startUpload();
        });
        await act(async () => {
            await result.current.reboot();
        });
        mockEnv({ client, device: null });
        rerender({});
        await waitFor(() => expect(result.current.step).toBe('reconnecting'));
        mockEnv({ client, device: { mac: 'AA:BB:CC' } });
        rerender({});

        await waitFor(() => expect(result.current.step).toBe('failed'));
        expect(client.confirmCurrentImage).not.toHaveBeenCalled();
        expect(result.current.error).toMatch(/restore the previous firmware/);
    });

    it('ignores a different device reconnecting while it waits', async () => {
        const client = makeClient(APP_HASH, APP_HASH);
        client.getImageState = jest
            .fn()
            .mockResolvedValueOnce({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH }],
            })
            .mockResolvedValue({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH, pending: true }],
            });
        mockEnv({ client, device: { mac: 'AA:BB:CC' } });

        const pkg = makePackage();
        const { result, rerender } = renderHook(() => useFirmwareUpdateFlow(pkg));
        await act(async () => {
            await result.current.startUpload();
        });
        await act(async () => {
            await result.current.reboot();
        });
        mockEnv({ client, device: null });
        rerender({});
        await waitFor(() => expect(result.current.step).toBe('reconnecting'));

        mockEnv({ client, device: { mac: 'ZZ:ZZ:ZZ' } });
        rerender({});

        expect(result.current.step).toBe('reconnecting');
    });

    it('reports upload progress and marks images uploaded', async () => {
        const client = makeClient(APP_HASH, APP_HASH);
        client.getImageState = jest
            .fn()
            .mockResolvedValueOnce({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH }],
            })
            .mockResolvedValue({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH, pending: true }],
            });
        mockEnv({ client, device: { mac: 'AA:BB:CC' } });

        const pkg = makePackage();
        const { result } = renderHook(() => useFirmwareUpdateFlow(pkg));
        await act(async () => {
            await result.current.startUpload();
        });

        expect(result.current.uploadProgress).toBe(100);
        expect(result.current.images[0].uploaded).toBe(true);
        expect(result.current.images[0].staged).toBe(true);
        expect(result.current.images[0].stagedHash).toBe('aabb');
    });

    it('surfaces an upload failure without rebooting anything', async () => {
        const client = makeClient(APP_HASH, APP_HASH);
        client.uploadImage = jest.fn(async (_d: Uint8Array, _i: number, _p?: any) => {
            throw new Error('SMP request timeout after 5000ms');
        }) as typeof client.uploadImage;
        mockEnv({ client, device: { mac: 'AA:BB:CC' } });

        const pkg = makePackage();
        const { result } = renderHook(() => useFirmwareUpdateFlow(pkg));
        await act(async () => {
            await result.current.startUpload();
        });

        expect(result.current.step).toBe('failed');
        expect(result.current.error).toMatch(/SMP request timeout/);
        expect(client.reset).not.toHaveBeenCalled();
    });
});
