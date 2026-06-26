import Constants from 'expo-constants';
import * as LegacyFS from 'expo-file-system/legacy';
import * as IntentLauncher from 'expo-intent-launcher';
import { Platform } from 'react-native';

import {
    compareVersions,
    fetchLatestAppRelease,
    findApkAsset,
    GitHubAsset,
    GitHubRelease,
    parseVersionFromTag,
} from './github-releases';

// The companion app and firmware live in the same monorepo; app releases are
// tagged `app-v*` (see .github/workflows/app-release.yml).
const REPO_OWNER = 'skalldri';
const REPO_NAME = 'rgb-sunglasses';

// Cached APK download target. Kept in the cache dir so the OS can reclaim it.
const APK_FILENAME = 'app-update.apk';

export interface AppUpdateInfo {
    /** Version of the available release, e.g. "1.2.3". */
    version: string;
    /** The full release (for notes, html_url, etc.). */
    release: GitHubRelease;
    /** The .apk asset to download (Android self-install). May be null on a malformed release. */
    apkAsset: GitHubAsset | null;
    /** Browser-facing release page (iOS deep-link fallback). */
    htmlUrl: string | undefined;
}

/**
 * The currently-installed app version, from the build-time Expo config (set into
 * the manifest by prebuild).
 *
 * `app.json` commits version "0.0.0"; the release workflow overwrites it with the
 * `app-v*` tag version before building. So dev builds report "0.0.0" — behind
 * every published release, which keeps the update check exercising the
 * download/install flow — while release builds report their real version.
 */
export function getCurrentAppVersion(): string {
    return Constants.expoConfig?.version ?? '0.0.0';
}

/**
 * Check GitHub Releases for a companion-app build newer than the installed one.
 * Returns null when up to date (or when no app release exists yet).
 *
 * Unauthenticated and one-shot — do not poll (GitHub rate-limits anonymous
 * requests to 60/hr per IP; see app/CLAUDE.md).
 */
export async function checkForAppUpdate(): Promise<AppUpdateInfo | null> {
    const release = await fetchLatestAppRelease(REPO_OWNER, REPO_NAME);
    if (!release) {
        return null;
    }

    const latestVersion = parseVersionFromTag(release.tag_name);
    const currentVersion = getCurrentAppVersion();
    if (compareVersions(latestVersion, currentVersion) <= 0) {
        return null; // up to date (or somehow behind)
    }

    return {
        version: latestVersion,
        release,
        apkAsset: findApkAsset(release.assets),
        htmlUrl: release.html_url,
    };
}

/**
 * Download an APK asset into the cache directory, reporting 0–100 progress.
 * Returns the local file URI. Uses the legacy file-system API because the
 * `next` API has no resumable-download primitive yet (same as the firmware
 * download path).
 */
export async function downloadApk(
    asset: GitHubAsset,
    onProgress?: (percent: number) => void
): Promise<string> {
    const destUri = (LegacyFS.cacheDirectory ?? '') + APK_FILENAME;

    const task = LegacyFS.createDownloadResumable(
        asset.browser_download_url,
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
    return result.uri;
}

/**
 * Launch the Android package installer for a downloaded APK. The system shows
 * its own confirmation dialog (and, on first use, prompts the user to grant
 * "install unknown apps" to this app). No-op on non-Android platforms.
 *
 * A `file://` URI can't be handed to another app on Android 7+, so we convert
 * to a `content://` URI via expo-file-system's bundled FileProvider and grant
 * read permission to the installer.
 */
export async function installApk(fileUri: string): Promise<void> {
    if (Platform.OS !== 'android') {
        throw new Error('APK install is only supported on Android');
    }

    const contentUri = await LegacyFS.getContentUriAsync(fileUri);
    await IntentLauncher.startActivityAsync('android.intent.action.INSTALL_PACKAGE', {
        data: contentUri,
        flags: 1, // FLAG_GRANT_READ_URI_PERMISSION
        type: 'application/vnd.android.package-archive',
    });
}
