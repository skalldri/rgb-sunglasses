import { act, renderHook, waitFor } from '@testing-library/react-native';
import React from 'react';

import * as BluetoothContext from '@/context/bluetooth-context';
import * as McuMgrClientContext from '@/context/mcumgr-client-context';
import { REBOOT_TIMEOUT_MS, useFirmwareUpdateFlow } from '@/hooks/use-firmware-update-flow';
import { FirmwarePackage } from '@/services/firmware-package';

import fixture from './fixtures/mcuboot-image-header.json';

/** Real MCUboot image bytes, so parseImageSha256 finds a genuine TLV digest. */
const APP_IMAGE = Uint8Array.from(fixture.bytes as number[]);
/** The digest inside APP_IMAGE — what the device must report for the staged slot. */
const APP_TLV_HASH = fixture.expectedSha256 as string;
const APP_TLV_BYTES = Uint8Array.from(
    (APP_TLV_HASH.match(/../g) as string[]).map(h => parseInt(h, 16))
);

/** A one-image package carrying a real signed image. */
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
                data: APP_IMAGE,
                parsedHeader: { magic: 0, version: '2.1.0+0', imageSize: 4 },
            },
        ],
    } as FirmwarePackage;
}

const APP_HASH = APP_TLV_BYTES;
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

    it('marks images permanent at staging (the bootloader is overwrite-only)', async () => {
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

        // confirm=true: this SoC's bootloader is overwrite-only, so there is no
        // test-then-confirm to have and the extra step would be a permanent no-op.
        expect(client.setImageState).toHaveBeenCalledWith(APP_HASH, true);
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
        expect(result.current.error).toMatch(/did not stage the uploaded image/);
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

    it('verifies the app core against the digest inside the zip', async () => {
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
        expect(result.current.images[0].verified).toBe(true);
    });

    it('fails when the running image is not the one that was uploaded', async () => {
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
        expect(result.current.error).toMatch(/not running the firmware that was uploaded/);
    });

    it('does not demand a hash match for the net core, whose hash changes on install', async () => {
        // Hardware-measured with fw-v2.1.0: the net-core image is a wrapper the app
        // core unwraps over IPC, so the installed hash (4d4b2c28…) differs from the
        // file's TLV (e43ebfa1…) even on a perfectly good update. Requiring a match
        // reported "Update failed" for an install that had in fact worked.
        const NET_FILE_HASH = Uint8Array.from([0xe4, 0x3e]);
        const NET_INSTALLED_HASH = Uint8Array.from([0x4d, 0x4b]);

        const twoImagePkg = {
            manifest: { 'format-version': 0, time: 0, name: 'fw', files: [] as any },
            images: [
                {
                    manifest: { file: 'fw.signed.bin', image_index: '0', size: 4 } as any,
                    data: APP_IMAGE,
                    parsedHeader: { magic: 0, version: '2.1.0+0', imageSize: 4 },
                },
                {
                    // No parseable TLV, so expectedHash is undefined for this one and
                    // verification must lean on the version alone.
                    manifest: { file: 'ipc_radio.bin', image_index: '1', size: 2 } as any,
                    data: Uint8Array.from([0, 0]),
                    parsedHeader: { magic: 0, version: '2.1.0+0', imageSize: 2 },
                },
            ],
        } as FirmwarePackage;

        const client = makeClient(APP_HASH, APP_HASH);
        client.getImageState = jest
            .fn()
            // staging reads, one per image
            .mockResolvedValueOnce({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH }],
            })
            .mockResolvedValueOnce({
                images: [{ image: 1, slot: 1, version: '2.1.0', hash: NET_FILE_HASH }],
            })
            .mockResolvedValueOnce({
                images: [{ image: 0, slot: 1, version: '2.1.0', hash: APP_HASH, pending: true }],
            })
            // post-reboot: net core reports a DIFFERENT hash, same version
            .mockResolvedValue({
                images: [
                    { image: 0, slot: 0, version: '2.1.0', hash: APP_HASH, active: true },
                    {
                        image: 1,
                        slot: 0,
                        version: '2.1.0',
                        hash: NET_INSTALLED_HASH,
                        active: true,
                    },
                ],
            });
        mockEnv({ client, device: { mac: 'AA:BB:CC' } });

        const { result, rerender } = renderHook(() => useFirmwareUpdateFlow(twoImagePkg));
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
        expect(result.current.images.map(i => i.verified)).toEqual([true, true]);
    });

    it('rejects a shorter running version that is only a prefix of the file version', async () => {
        // '2.1.10+0'.startsWith('2.1.1') is true, so a bare prefix match verified a
        // device stuck on 2.1.1 as an update to 2.1.10 - a false success in exactly the
        // step that exists to catch a failed install. Uses image 1 (net core), which is
        // exempt from the hash check, so the version comparison is the only signal.
        const netPkg = {
            manifest: { 'format-version': 0, time: 0, name: 'fw', files: [] as any },
            images: [
                {
                    manifest: { file: 'ipc_radio.bin', image_index: '1', size: 2 } as any,
                    data: Uint8Array.from([0, 0]),
                    parsedHeader: { magic: 0, version: '2.1.10+0', imageSize: 2 },
                },
            ],
        } as FirmwarePackage;

        const client = makeClient(APP_HASH, APP_HASH);
        client.getImageState = jest
            .fn()
            .mockResolvedValueOnce({
                images: [{ image: 1, slot: 1, version: '2.1.10', hash: APP_HASH }],
            })
            .mockResolvedValueOnce({
                images: [{ image: 1, slot: 1, version: '2.1.10', hash: APP_HASH, pending: true }],
            })
            // The net core failed to install and is still on the older 2.1.1.
            .mockResolvedValue({
                images: [{ image: 1, slot: 0, version: '2.1.1', hash: APP_HASH, active: true }],
            });
        mockEnv({ client, device: { mac: 'AA:BB:CC' } });

        const { result, rerender } = renderHook(() => useFirmwareUpdateFlow(netPkg));
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
    });

    it('accepts the file version with its +build suffix against the device version', async () => {
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

        const pkg = makePackage(); // header version '2.1.0+0'
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
    });

    it('fails instead of spinning forever when the restart never drops the link', async () => {
        // reset() swallows its own rejection, so a reset that never reached the device
        // is indistinguishable from one that did. Without a timeout the user sat on
        // "Restarting…" forever with no counter and no way back.
        jest.useFakeTimers();
        try {
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
            await act(async () => {
                await result.current.reboot();
            });
            expect(result.current.step).toBe('rebooting');

            // Device stays connected — the reset did not take.
            await act(async () => {
                jest.advanceTimersByTime(REBOOT_TIMEOUT_MS + 1000);
            });

            expect(result.current.step).toBe('failed');
            expect(result.current.error).toMatch(/did not restart/);
        } finally {
            jest.useRealTimers();
        }
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
        expect(result.current.images[0].stagedHash).toBe(APP_TLV_HASH);
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
