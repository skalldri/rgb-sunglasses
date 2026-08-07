import {
    EXTENSION_DIRECTORY,
    countDeviceExtensions,
    countUnmanagedExtensions,
    entriesNeedingUpload,
    extensionPathFor,
    findExtensionAssets,
    isValidExtensionAssetName,
    MAX_EXTENSION_NAME_LENGTH,
    parseAssetSha256,
    planExtensionSync,
    syncExtensions,
} from '@/services/extension-sync';
import type { GitHubAsset } from '@/services/github-releases';
import { FsMgmtError, SmpCommandError, SmpGroup } from '@/services/mcumgr';
import { sha256 } from 'js-sha256';

const HASH_A = 'a'.repeat(64);
const HASH_B = 'b'.repeat(64);

function asset(name: string, digest?: string): GitHubAsset {
    return {
        id: 1,
        name,
        browser_download_url: `https://example.test/${name}`,
        size: 1234,
        content_type: 'application/octet-stream',
        digest,
    };
}

describe('parseAssetSha256', () => {
    it('extracts the hex digest from a sha256-prefixed value', () => {
        expect(parseAssetSha256(`sha256:${HASH_A}`)).toBe(HASH_A);
    });

    it('lowercases the digest so comparisons are case-insensitive', () => {
        expect(parseAssetSha256(`sha256:${'A'.repeat(64)}`)).toBe('a'.repeat(64));
    });

    it('tolerates surrounding whitespace', () => {
        expect(parseAssetSha256(`  sha256:${HASH_A}  `)).toBe(HASH_A);
    });

    it('returns null for a missing, empty, non-sha256 or malformed digest', () => {
        expect(parseAssetSha256(undefined)).toBeNull();
        expect(parseAssetSha256(null)).toBeNull();
        expect(parseAssetSha256('')).toBeNull();
        expect(parseAssetSha256(`md5:${'a'.repeat(32)}`)).toBeNull();
        expect(parseAssetSha256(HASH_A)).toBeNull(); // no algorithm prefix
        expect(parseAssetSha256('sha256:nothex')).toBeNull();
        expect(parseAssetSha256(`sha256:${'a'.repeat(63)}`)).toBeNull(); // too short
    });
});

describe('isValidExtensionAssetName', () => {
    it('accepts a plain .llext file name', () => {
        expect(isValidExtensionAssetName('plasma.llext')).toBe(true);
    });

    it('rejects non-.llext assets', () => {
        expect(isValidExtensionAssetName('dfu_application_proto0.zip')).toBe(false);
        expect(isValidExtensionAssetName('app-release.apk')).toBe(false);
    });

    it('rejects names that would escape the extension directory', () => {
        // The firmware fences this too, but a malformed asset name must never be
        // concatenated into a path that means something else.
        expect(isValidExtensionAssetName('../mcuboot.llext')).toBe(false);
        expect(isValidExtensionAssetName('sub/plasma.llext')).toBe(false);
        expect(isValidExtensionAssetName('sub\\plasma.llext')).toBe(false);
        expect(isValidExtensionAssetName('.llext')).toBe(false);
    });

    it('rejects names longer than the firmware can store', () => {
        // extension_registry copies discovered names into a 32-byte buffer, so a
        // longer name would upload, re-hash and report "Up to date" while the
        // firmware silently failed to load a truncated path at next boot.
        const at = 'a'.repeat(MAX_EXTENSION_NAME_LENGTH - '.llext'.length) + '.llext';
        expect(at.length).toBe(MAX_EXTENSION_NAME_LENGTH);
        expect(isValidExtensionAssetName(at)).toBe(true);

        const over = 'a'.repeat(MAX_EXTENSION_NAME_LENGTH - '.llext'.length + 1) + '.llext';
        expect(over.length).toBe(MAX_EXTENSION_NAME_LENGTH + 1);
        expect(isValidExtensionAssetName(over)).toBe(false);

        expect(isValidExtensionAssetName('spectrum_analyzer_bars_hires.llext')).toBe(false);
    });
});

describe('findExtensionAssets', () => {
    it('keeps only the .llext assets, in release order', () => {
        const assets = [
            asset('dfu_application_proto0.zip'),
            asset('hello.llext'),
            asset('plasma.llext'),
        ];
        expect(findExtensionAssets(assets).map(a => a.name)).toEqual([
            'hello.llext',
            'plasma.llext',
        ]);
    });
});

describe('extensionPathFor', () => {
    it('builds a path under the firmware extension directory', () => {
        expect(extensionPathFor('plasma.llext')).toBe(`${EXTENSION_DIRECTORY}/plasma.llext`);
        expect(extensionPathFor('plasma.llext')).toBe('/NAND:/ext/plasma.llext');
    });
});

describe('planExtensionSync', () => {
    function clientWithHashes(hashes: Record<string, string | null>) {
        return {
            getFileSha256: jest.fn(async (path: string) => hashes[path] ?? null),
        } as any;
    }

    it('classifies matching, differing and absent files', async () => {
        const client = clientWithHashes({
            '/NAND:/ext/hello.llext': HASH_A,
            '/NAND:/ext/plasma.llext': HASH_B,
            // newfx.llext deliberately absent
        });
        const assets = [
            asset('hello.llext', `sha256:${HASH_A}`),
            asset('plasma.llext', `sha256:${HASH_A}`),
            asset('newfx.llext', `sha256:${HASH_A}`),
        ];

        const plan = await planExtensionSync(client, assets);

        expect(plan.map(e => [e.name, e.status])).toEqual([
            ['hello.llext', 'up-to-date'],
            ['plasma.llext', 'outdated'],
            ['newfx.llext', 'missing'],
        ]);
    });

    it('compares digests case-insensitively', async () => {
        const client = clientWithHashes({ '/NAND:/ext/hello.llext': HASH_A });
        const plan = await planExtensionSync(client, [
            asset('hello.llext', `sha256:${'A'.repeat(64)}`),
        ]);
        expect(plan[0].status).toBe('up-to-date');
    });

    it('leaves a file alone when the release publishes no digest', async () => {
        // Guessing "outdated" here would re-upload an identical file on every
        // single update check.
        const client = clientWithHashes({ '/NAND:/ext/hello.llext': HASH_A });
        const plan = await planExtensionSync(client, [asset('hello.llext', undefined)]);

        expect(plan[0].status).toBe('up-to-date');
        expect(plan[0].expectedSha256).toBeNull();
    });

    it('still installs a missing file when the release publishes no digest', async () => {
        const client = clientWithHashes({});
        const plan = await planExtensionSync(client, [asset('newfx.llext', undefined)]);
        expect(plan[0].status).toBe('missing');
    });

    it('ignores non-extension assets', async () => {
        const client = clientWithHashes({});
        const plan = await planExtensionSync(client, [
            asset('dfu_application_proto0.zip', `sha256:${HASH_A}`),
        ]);

        expect(plan).toEqual([]);
        expect(client.getFileSha256).not.toHaveBeenCalled();
    });

    it('marks a file the device cannot hash as needing repair, without failing the plan', async () => {
        // fs_mgmt answers a hash request for a zero-length file with FILE_EMPTY
        // (16), not FILE_NOT_FOUND - the state an interrupted upload leaves
        // behind. One bad file must not make every extension unsyncable.
        const client = {
            getFileSha256: jest.fn(async (path: string) => {
                if (path.includes('plasma')) {
                    throw new SmpCommandError(
                        'File hash error: group=8, rc=16',
                        FsMgmtError.FILE_EMPTY,
                        SmpGroup.FS
                    );
                }
                return HASH_A;
            }),
        } as any;

        const plan = await planExtensionSync(client, [
            asset('hello.llext', `sha256:${HASH_A}`),
            asset('plasma.llext', `sha256:${HASH_A}`),
        ]);

        expect(plan.map(e => [e.name, e.status])).toEqual([
            ['hello.llext', 'up-to-date'],
            ['plasma.llext', 'unreadable'],
        ]);
        expect(plan[1].deviceError).toContain('rc=16');
        expect(entriesNeedingUpload(plan).map(e => e.name)).toEqual(['plasma.llext']);
    });

    it('propagates an error that is not attributable to the FS group', async () => {
        // A transport timeout, or ENOTSUP from firmware with no file management,
        // is a whole-feature failure - reporting "needs upload" would start an
        // upload that cannot succeed.
        const client = {
            getFileSha256: jest.fn(async () => {
                throw new SmpCommandError('File hash error: rc=8', 8, undefined);
            }),
        } as any;

        await expect(
            planExtensionSync(client, [asset('hello.llext', `sha256:${HASH_A}`)])
        ).rejects.toThrow('File hash error');
    });

    it('propagates a plain transport error', async () => {
        const client = {
            getFileSha256: jest.fn(async () => {
                throw new Error('SMP request timeout after 5000ms');
            }),
        } as any;

        await expect(
            planExtensionSync(client, [asset('hello.llext', `sha256:${HASH_A}`)])
        ).rejects.toThrow('SMP request timeout');
    });
});

describe('entriesNeedingUpload', () => {
    it('selects everything that is not already up to date', () => {
        const entries = [
            { name: 'a', status: 'up-to-date' },
            { name: 'b', status: 'outdated' },
            { name: 'c', status: 'missing' },
        ] as any;
        expect(entriesNeedingUpload(entries).map((e: any) => e.name)).toEqual(['b', 'c']);
    });
});

describe('countDeviceExtensions', () => {
    // Extension slots are animation ids 0x40..0x4f, landing in the 4th UUID group
    // shifted left by 8 (animationServiceUuidForId).
    const extensionSlot0 = '12345678-1234-5678-4000-56789abd0000';
    const extensionSlot1 = '12345678-1234-5678-4100-56789abd0000';
    const extensionSlot15 = '12345678-1234-5678-4f00-56789abd0000';
    const builtInRainbow = '12345678-1234-5678-0500-56789abd0000';
    const coreConfig = '12345678-1234-5678-0001-56789abc0000';

    it('counts only extension animation services', () => {
        expect(
            countDeviceExtensions([coreConfig, builtInRainbow, extensionSlot0, extensionSlot1])
        ).toBe(2);
    });

    it('covers the whole extension slot range', () => {
        expect(countDeviceExtensions([extensionSlot0, extensionSlot15])).toBe(2);
    });

    it('excludes built-in animations and non-animation services', () => {
        expect(countDeviceExtensions([coreConfig, builtInRainbow])).toBe(0);
    });

    it('ignores malformed UUIDs rather than counting them', () => {
        expect(countDeviceExtensions(['not-a-uuid', ''])).toBe(0);
    });
});

describe('countUnmanagedExtensions', () => {
    const present = (name: string) =>
        ({ name, deviceSha256: HASH_A, status: 'up-to-date' } as any);
    const absent = (name: string) => ({ name, deviceSha256: null, status: 'missing' } as any);

    it('reports the device extensions this release does not account for', () => {
        // Device runs 3 extensions; the release ships 2 and both are present, so
        // the third is unmanaged.
        expect(countUnmanagedExtensions(3, [present('a'), present('b')])).toBe(1);
    });

    it('does not count a released extension that is missing from the device', () => {
        // Device runs 1 extension, release ships 2 of which only one is installed.
        expect(countUnmanagedExtensions(1, [present('a'), absent('b')])).toBe(0);
    });

    it('returns zero when every device extension came from this release', () => {
        expect(countUnmanagedExtensions(2, [present('a'), present('b')])).toBe(0);
    });

    it('never goes negative', () => {
        // Can happen transiently: a file exists on disk but failed to load, so it
        // has a hash but no service.
        expect(countUnmanagedExtensions(0, [present('a')])).toBe(0);
    });
});

describe('syncExtensions', () => {
    function makeClient() {
        return { uploadFile: jest.fn(async () => undefined) } as any;
    }

    const entries = [
        {
            name: 'hello.llext',
            path: '/NAND:/ext/hello.llext',
            status: 'up-to-date',
            asset: asset('hello.llext'),
        },
        {
            name: 'plasma.llext',
            path: '/NAND:/ext/plasma.llext',
            status: 'outdated',
            asset: asset('plasma.llext'),
        },
        {
            name: 'newfx.llext',
            path: '/NAND:/ext/newfx.llext',
            status: 'missing',
            asset: asset('newfx.llext'),
        },
    ] as any;

    it('uploads only the entries that need it, to their device paths', async () => {
        const client = makeClient();
        const download = jest.fn(async () => new Uint8Array([1, 2, 3]));

        const uploaded = await syncExtensions(client, entries, download);

        expect(uploaded.map((e: any) => e.name)).toEqual(['plasma.llext', 'newfx.llext']);
        expect(client.uploadFile.mock.calls.map((c: any[]) => c[0])).toEqual([
            '/NAND:/ext/plasma.llext',
            '/NAND:/ext/newfx.llext',
        ]);
    });

    it('does not download an up-to-date extension', async () => {
        const client = makeClient();
        const download = jest.fn(async () => new Uint8Array([1]));

        await syncExtensions(client, entries, download);

        expect(download).toHaveBeenCalledTimes(2);
        expect(download.mock.calls.map((c: any[]) => c[0].name)).toEqual([
            'plasma.llext',
            'newfx.llext',
        ]);
    });

    it('downloads each file immediately before its own upload', async () => {
        // A failure part-way through should leave a coherent prefix of work done,
        // not a pile of downloads for uploads that never happened.
        const order: string[] = [];
        const client = {
            uploadFile: jest.fn(async (path: string) => {
                order.push(`upload:${path}`);
            }),
        } as any;
        const download = jest.fn(async (a: GitHubAsset) => {
            order.push(`download:${a.name}`);
            return new Uint8Array([1]);
        });

        await syncExtensions(client, entries, download);

        expect(order).toEqual([
            'download:plasma.llext',
            'upload:/NAND:/ext/plasma.llext',
            'download:newfx.llext',
            'upload:/NAND:/ext/newfx.llext',
        ]);
    });

    it('reports progress against the number of entries actually being uploaded', async () => {
        const client = {
            uploadFile: jest.fn(async (_path: string, _data: Uint8Array, onProgress: any) => {
                onProgress(50, 100);
                onProgress(100, 100);
            }),
        } as any;
        const progress: string[] = [];

        await syncExtensions(client, entries, async () => new Uint8Array([1]), p => {
            progress.push(`${p.entry.name} ${p.index + 1}/${p.total} ${p.bytesSent}/${p.bytesTotal}`);
        });

        expect(progress).toEqual([
            'plasma.llext 1/2 50/100',
            'plasma.llext 1/2 100/100',
            'newfx.llext 2/2 50/100',
            'newfx.llext 2/2 100/100',
        ]);
    });

    it('refuses to upload bytes that do not match the release digest', async () => {
        // A truncated CDN response would otherwise overwrite a working extension
        // and only fail at the next boot.
        const client = makeClient();
        const withDigest = [
            {
                name: 'plasma.llext',
                path: '/NAND:/ext/plasma.llext',
                status: 'outdated',
                asset: asset('plasma.llext'),
                expectedSha256: HASH_A,
            },
        ] as any;

        await expect(
            syncExtensions(client, withDigest, async () => Uint8Array.from([1, 2, 3]))
        ).rejects.toThrow(/does not match the release digest/);
        expect(client.uploadFile).not.toHaveBeenCalled();
    });

    it('uploads when the downloaded bytes match the release digest', async () => {
        const client = makeClient();
        const bytes = Uint8Array.from([1, 2, 3]);
        const entry = [
            {
                name: 'plasma.llext',
                path: '/NAND:/ext/plasma.llext',
                status: 'outdated',
                asset: asset('plasma.llext'),
                expectedSha256: sha256.hex(bytes),
            },
        ] as any;

        await syncExtensions(client, entry, async () => bytes);
        expect(client.uploadFile).toHaveBeenCalledTimes(1);
    });

    it('uploads without verification when the release published no digest', async () => {
        // Older releases legitimately lack a digest; that must not block a sync.
        const client = makeClient();
        const entry = [
            {
                name: 'plasma.llext',
                path: '/NAND:/ext/plasma.llext',
                status: 'missing',
                asset: asset('plasma.llext'),
                expectedSha256: null,
            },
        ] as any;

        await syncExtensions(client, entry, async () => Uint8Array.from([7]));
        expect(client.uploadFile).toHaveBeenCalledTimes(1);
    });

    it('stops at the first failure, leaving later entries untouched', async () => {
        const client = {
            uploadFile: jest.fn(async (path: string) => {
                if (path.includes('plasma')) throw new Error('File upload error at offset 0');
            }),
        } as any;

        await expect(
            syncExtensions(client, entries, async () => new Uint8Array([1]))
        ).rejects.toThrow('File upload error');

        expect(client.uploadFile).toHaveBeenCalledTimes(1);
    });
});
