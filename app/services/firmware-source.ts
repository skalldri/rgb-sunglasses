import { FirmwarePackage, parseFirmwarePackageFromBase64 } from '@/services/firmware-package';
import * as LegacyFS from 'expo-file-system/legacy';
import { File } from 'expo-file-system/next';

/**
 * Where a firmware package is coming from.
 *
 * The guided flow lives on its own screen, and navigation params are strings only —
 * a parsed `FirmwarePackage` holds `Uint8Array` image data and cannot travel that
 * way. So the landing page passes a *descriptor* and the flow re-derives the package
 * from it. Both variants are cheap to re-resolve: a picked file is already in the
 * app's cache directory, and a release download is a plain HTTPS GET.
 */
export type FirmwareSource =
    | { kind: 'file'; uri: string; name?: string }
    | { kind: 'release'; url: string; version?: string };

export type DownloadProgressCallback = (percent: number) => void;

/** Reads a local `.zip` (already in cache, e.g. from DocumentPicker) into a package. */
export async function loadPackageFromFile(uri: string): Promise<FirmwarePackage> {
    const base64Data = await new File(uri).base64();
    return parseFirmwarePackageFromBase64(base64Data);
}

/**
 * Downloads a release asset and parses it.
 *
 * Uses `expo-file-system/legacy`'s resumable download rather than the newer `next`
 * File API because only the legacy one reports progress, and a ~850 KB download over
 * a phone connection is long enough to need a progress bar.
 */
export async function loadPackageFromRelease(
    url: string,
    onProgress?: DownloadProgressCallback
): Promise<FirmwarePackage> {
    const destUri = (LegacyFS.cacheDirectory ?? '') + 'firmware-update.zip';

    const task = LegacyFS.createDownloadResumable(
        url,
        destUri,
        {},
        ({
            totalBytesWritten,
            totalBytesExpectedToWrite,
        }: {
            totalBytesWritten: number;
            totalBytesExpectedToWrite: number;
        }) => {
            if (totalBytesExpectedToWrite > 0) {
                onProgress?.(Math.round((totalBytesWritten / totalBytesExpectedToWrite) * 100));
            }
        }
    );

    const result = await task.downloadAsync();
    if (!result) {
        throw new Error('Download was cancelled');
    }
    return loadPackageFromFile(result.uri);
}

/** Resolves either source kind to a parsed package. */
export function loadPackage(
    source: FirmwareSource,
    onProgress?: DownloadProgressCallback
): Promise<FirmwarePackage> {
    return source.kind === 'file'
        ? loadPackageFromFile(source.uri)
        : loadPackageFromRelease(source.url, onProgress);
}
