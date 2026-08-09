/**
 * Animation-extension sync
 *
 * Animation extensions are `.llext` files the firmware discovers at boot from
 * `/NAND:/ext`. They ship as bare assets on the same GitHub release as the
 * firmware image, but until now the only way onto the board was a manual USB
 * mass-storage copy — so a device that took an OTA update kept whatever
 * extensions it already had, including ones built against an older RGBX ABI.
 *
 * This module works out which files need replacing and does it over MCUmgr's FS
 * group. The comparison is SHA256: the device hashes its own copy (see
 * `getFileSha256`), and GitHub already reports a `sha256:` digest for every
 * release asset, so no sidecar manifest is needed.
 *
 * Deliberately free of React and of BLE plumbing so the decision logic is
 * directly testable.
 */

import { sha256 } from 'js-sha256';
import { GitHubAsset } from './github-releases';
import { isSmpGroupError, McuMgrClient, SmpGroup } from './mcumgr';

/** Lowercase hex SHA256 of a byte array, matching GitHub's digest format. */
function sha256Hex(data: Uint8Array): string {
    return sha256.hex(data);
}

/**
 * Directory the firmware scans for extensions, mirroring
 * `extension_registry::kDirectory`. The firmware fences every MCUmgr file
 * operation to this directory (`extension_file_transfer.cpp`), so a path built
 * any other way will be rejected by the device rather than silently written
 * somewhere else.
 */
export const EXTENSION_DIRECTORY = '/NAND:/ext';

/** File suffix identifying an extension asset on a release. */
export const EXTENSION_FILE_SUFFIX = '.llext';

export type ExtensionSyncStatus =
    /** Device's copy already matches the release. */
    | 'up-to-date'
    /** Device has this file, but with different contents. */
    | 'outdated'
    /** Device does not have this file at all. */
    | 'missing'
    /**
     * The file is on the device but could not be hashed - most commonly a
     * zero-length file left by an upload that was interrupted part-way.
     * Treated as needing upload, which is what makes a half-finished sync
     * self-healing rather than permanently broken.
     */
    | 'unreadable';

export interface ExtensionSyncEntry {
    /** File name as published on the release, e.g. "plasma.llext". */
    name: string;
    /** Absolute on-device path, e.g. "/NAND:/ext/plasma.llext". */
    path: string;
    status: ExtensionSyncStatus;
    /** The release asset this entry came from. */
    asset: GitHubAsset;
    /** Lowercase hex SHA256 the release reports, or null if it reported none. */
    expectedSha256: string | null;
    /**
     * Lowercase hex SHA256 the device reported, or null if the file is absent
     * or could not be hashed.
     */
    deviceSha256: string | null;
    /** Device-side error that made the file unreadable, if status is 'unreadable'. */
    deviceError?: string;
}

/**
 * Extract the lowercase hex digest from a GitHub asset's `digest` field
 * ("sha256:<hex>"), or null if absent or not a sha256.
 *
 * GitHub added this field relatively recently, so an older release — or a
 * future change of algorithm — can legitimately produce null. Callers must
 * treat that as "can't compare", never as "hashes differ": re-uploading every
 * extension on every update check because a digest was missing would be a
 * silent, repeated waste of the user's time and battery.
 */
export function parseAssetSha256(digest: string | undefined | null): string | null {
    if (!digest) return null;
    const match = /^sha256:([0-9a-fA-F]{64})$/.exec(digest.trim());
    return match ? match[1].toLowerCase() : null;
}

/**
 * Longest extension file name the firmware can actually use.
 *
 * `extension_registry::kMaxNameLen` is a 32-byte buffer that discovered names
 * are `strncpy`'d into, so 31 characters is the real ceiling. A longer name is
 * worse than a hard failure: the upload, the re-hash and the "Up to date" badge
 * all succeed, then at next boot the registry stores a truncated name,
 * `full_path()` builds a path that doesn't exist, and the extension silently
 * never loads.
 */
export const MAX_EXTENSION_NAME_LENGTH = 31;

/**
 * True if `name` is a plausible extension file name that can be turned into an
 * on-device path the firmware can actually use.
 *
 * The firmware rejects anything outside its extension directory anyway, so the
 * path-shape checks are defence in depth rather than the security boundary —
 * but the length check is load-bearing (see MAX_EXTENSION_NAME_LENGTH), and
 * keeping a malformed name from being concatenated into a path that means
 * something else entirely is cheap.
 */
export function isValidExtensionAssetName(name: string): boolean {
    if (!name.endsWith(EXTENSION_FILE_SUFFIX)) return false;
    if (name.length === EXTENSION_FILE_SUFFIX.length) return false; // bare ".llext"
    if (name.length > MAX_EXTENSION_NAME_LENGTH) return false;
    if (name.includes('/') || name.includes('\\')) return false;
    if (name.includes('..')) return false;
    return true;
}

/** The `.llext` assets of a release, in the order the release lists them. */
export function findExtensionAssets(assets: GitHubAsset[]): GitHubAsset[] {
    return assets.filter(a => isValidExtensionAssetName(a.name));
}

/** Absolute on-device path for an extension file name. */
export function extensionPathFor(name: string): string {
    return `${EXTENSION_DIRECTORY}/${name}`;
}

/**
 * Ask the device for the SHA256 of every extension the release ships, and
 * classify each one.
 *
 * Reads are sequential on purpose: `McuMgrClient` serializes every exchange
 * onto one request chain anyway, so issuing them concurrently would buy nothing
 * and only make failures harder to attribute.
 */
export async function planExtensionSync(
    client: McuMgrClient,
    assets: GitHubAsset[]
): Promise<ExtensionSyncEntry[]> {
    const entries: ExtensionSyncEntry[] = [];

    for (const asset of findExtensionAssets(assets)) {
        const path = extensionPathFor(asset.name);
        const expectedSha256 = parseAssetSha256(asset.digest);

        let deviceSha256: string | null = null;
        let deviceError: string | undefined;
        try {
            deviceSha256 = await client.getFileSha256(path);
        } catch (e: unknown) {
            // A FS-group error is about THIS file, so degrade this one entry to
            // "needs upload" instead of killing the whole plan. The case that
            // matters: an upload interrupted by a BLE drop leaves a zero-length
            // file, and Zephyr's fs_mgmt answers a hash request for it with
            // FILE_EMPTY (16), not FILE_NOT_FOUND - so without this, one
            // truncated file made every extension unsyncable and could only be
            // repaired over USB.
            //
            // Anything that is NOT a FS-group error (a transport timeout, or
            // ENOTSUP from firmware with no file management at all) still
            // propagates: those are whole-feature failures, and silently
            // reporting "needs upload" for them would send us into an upload
            // that cannot succeed.
            if (!isSmpGroupError(e, SmpGroup.FS)) {
                throw e;
            }
            deviceError = e instanceof Error ? e.message : String(e);
        }

        entries.push({
            name: asset.name,
            path,
            asset,
            expectedSha256,
            deviceSha256,
            deviceError,
            status: deviceError
                ? 'unreadable'
                : classifyExtension(expectedSha256, deviceSha256),
        });
    }

    return entries;
}

function classifyExtension(
    expectedSha256: string | null,
    deviceSha256: string | null
): ExtensionSyncStatus {
    if (deviceSha256 === null) return 'missing';
    // No digest published: we can't tell whether it differs, and guessing
    // "outdated" would re-upload an identical file on every check. Leave it be.
    if (expectedSha256 === null) return 'up-to-date';
    return expectedSha256 === deviceSha256 ? 'up-to-date' : 'outdated';
}

/** Entries that actually need uploading. */
export function entriesNeedingUpload(entries: ExtensionSyncEntry[]): ExtensionSyncEntry[] {
    return entries.filter(e => e.status !== 'up-to-date');
}

// The old `countDeviceExtensions`/`countUnmanagedExtensions` heuristic — count
// GATT extension services, subtract release-shipped files the device could hash,
// valid only when both inputs describe the same boot — is gone: the FILE_MGMT
// LIST (`McuMgrClient.listDeviceFiles`, services/extension-management.ts) names
// every device file directly, including the loaded-but-deleted state the count
// could only imply.

export interface ExtensionSyncProgress {
    /** Entry currently being uploaded. */
    entry: ExtensionSyncEntry;
    /** 0-based index within the list of entries needing upload. */
    index: number;
    /** How many entries need uploading in total. */
    total: number;
    /** Bytes of this file sent so far, and its total size. */
    bytesSent: number;
    bytesTotal: number;
}

export type ExtensionDownloader = (asset: GitHubAsset) => Promise<Uint8Array>;

/**
 * Default downloader: fetch an extension asset straight into memory.
 *
 * Plain `fetch` rather than the resumable file download the firmware zip uses —
 * extensions are a few kilobytes, so streaming them to disk and reading them
 * back would be more moving parts for no benefit.
 */
export async function downloadExtensionAsset(asset: GitHubAsset): Promise<Uint8Array> {
    const response = await fetch(asset.browser_download_url);
    if (!response.ok) {
        throw new Error(
            `Failed to download ${asset.name}: ${response.status} ${response.statusText}`
        );
    }
    return new Uint8Array(await response.arrayBuffer());
}

/**
 * Upload every entry that needs it, in order.
 *
 * Each file is downloaded immediately before its upload rather than all of them
 * up front, so a failure part-way through leaves the device with a coherent
 * prefix of the work done and doesn't strand a pile of unused downloads.
 *
 * @returns the entries that were successfully uploaded.
 */
export async function syncExtensions(
    client: McuMgrClient,
    entries: ExtensionSyncEntry[],
    download: ExtensionDownloader,
    onProgress?: (progress: ExtensionSyncProgress) => void
): Promise<ExtensionSyncEntry[]> {
    const pending = entriesNeedingUpload(entries);
    const uploaded: ExtensionSyncEntry[] = [];

    for (let index = 0; index < pending.length; index++) {
        const entry = pending[index];
        const data = await download(entry.asset);

        // Verify what we downloaded before writing it over a file that currently
        // works. A truncated body (fetch can resolve short on a flaky redirected
        // CDN response) would otherwise be uploaded, report success, and only
        // surface at the next boot as an extension that silently stopped
        // loading. The digest is already in hand, so this costs one local hash.
        if (entry.expectedSha256) {
            const actual = sha256Hex(data);
            if (actual !== entry.expectedSha256) {
                throw new Error(
                    `Downloaded ${entry.name} does not match the release digest ` +
                        `(expected ${entry.expectedSha256}, got ${actual}) - not uploading`
                );
            }
        }

        await client.uploadFile(entry.path, data, (bytesSent, bytesTotal) => {
            onProgress?.({ entry, index, total: pending.length, bytesSent, bytesTotal });
        });

        uploaded.push(entry);
    }

    return uploaded;
}
