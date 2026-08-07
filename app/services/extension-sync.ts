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

import { GitHubAsset } from './github-releases';
import { McuMgrClient } from './mcumgr';

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
    | 'missing';

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
    /** Lowercase hex SHA256 the device reported, or null if the file is absent. */
    deviceSha256: string | null;
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
 * True if `name` is a plausible extension file name that can be turned into an
 * on-device path.
 *
 * The firmware rejects anything outside its extension directory anyway, so this
 * is defence in depth rather than the security boundary — but it keeps a
 * malformed asset name from being concatenated into a path that means something
 * else entirely.
 */
export function isValidExtensionAssetName(name: string): boolean {
    if (!name.endsWith(EXTENSION_FILE_SUFFIX)) return false;
    if (name.length === EXTENSION_FILE_SUFFIX.length) return false; // bare ".llext"
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
        const deviceSha256 = await client.getFileSha256(path);

        entries.push({
            name: asset.name,
            path,
            asset,
            expectedSha256,
            deviceSha256,
            status: classifyExtension(expectedSha256, deviceSha256),
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

/**
 * Firmware animation-id range for extension slots: `kAnimationIdBase` (0x40)
 * through `kAnimationIdBase + kMaxExtensions - 1`, per
 * `fw/src/extensions/extension_limits.h`.
 */
const EXTENSION_ANIMATION_ID_MIN = 0x40;
const EXTENSION_ANIMATION_ID_MAX = 0x4f;

/**
 * How many extension animation services the device exposed at connect time —
 * i.e. how many `.llext` files the firmware successfully loaded from
 * `/NAND:/ext`.
 *
 * Derived from the service UUIDs, whose 4th group is `animationId << 8`
 * (`animationServiceUuidForId` in constants/bluetooth.ts).
 */
export function countDeviceExtensions(serviceUuids: string[]): number {
    return serviceUuids.filter(uuid => {
        const group = uuid.split('-')[3];
        if (!group) return false;
        const animationId = parseInt(group, 16) >> 8;
        return (
            animationId >= EXTENSION_ANIMATION_ID_MIN && animationId <= EXTENSION_ANIMATION_ID_MAX
        );
    }).length;
}

/**
 * How many extensions the device is running that this release does not ship.
 *
 * MCUmgr's FS group has no delete command and no way to list a directory, so
 * these can neither be removed nor named over BLE — the count is surfaced so
 * the user knows to look, and removal is a USB mass-storage job.
 *
 * Counting rather than naming is deliberate. The device's extension *services*
 * carry the manifest display name ("Hello"), while the release ships *file*
 * names ("hello.llext"), and nothing guarantees those correspond — so matching
 * them up would produce a confident-looking list that is sometimes simply
 * wrong. The count, by contrast, is exact: every loaded extension gets one
 * service, and `planExtensionSync` already knows which released files the
 * device actually has.
 *
 * @param deviceExtensionCount from countDeviceExtensions()
 * @param entries the plan from planExtensionSync()
 */
export function countUnmanagedExtensions(
    deviceExtensionCount: number,
    entries: ExtensionSyncEntry[]
): number {
    const releasedAndPresent = entries.filter(e => e.deviceSha256 !== null).length;
    return Math.max(0, deviceExtensionCount - releasedAndPresent);
}

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

        await client.uploadFile(entry.path, data, (bytesSent, bytesTotal) => {
            onProgress?.({ entry, index, total: pending.length, bytesSent, bytesTotal });
        });

        uploaded.push(entry);
    }

    return uploaded;
}
