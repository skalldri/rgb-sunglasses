jest.mock('expo-constants', () => ({
  __esModule: true,
  default: { expoConfig: { version: '0.0.0' } },
}));

import Constants from 'expo-constants';
import * as LegacyFS from 'expo-file-system/legacy';
import * as IntentLauncher from 'expo-intent-launcher';
import { Platform } from 'react-native';

import { checkForAppUpdate, downloadApk, getCurrentAppVersion, installApk } from '@/services/app-update';
import { GitHubRelease } from '@/services/github-releases';

function appRelease(tag: string, opts: { apk?: boolean; html_url?: string } = {}): GitHubRelease {
  const { apk = true, html_url = `https://github.com/skalldri/rgb-sunglasses/releases/tag/${tag}` } = opts;
  return {
    id: 1,
    tag_name: tag,
    name: tag,
    published_at: '2026-01-01T00:00:00Z',
    html_url,
    assets: apk
      ? [
          {
            id: 1,
            name: `rgb-sunglasses-${tag.replace('app-v', '')}.apk`,
            browser_download_url: `https://example.com/${tag}.apk`,
            size: 1,
            content_type: 'application/vnd.android.package-archive',
          },
        ]
      : [],
  };
}

const originalFetch = global.fetch;

function mockReleases(releases: GitHubRelease[]) {
  let call = 0;
  global.fetch = jest.fn(async () => {
    const body = call === 0 ? releases : [];
    call++;
    return { ok: true, status: 200, statusText: 'OK', json: async () => body };
  }) as unknown as typeof fetch;
}

// Set the build-time Expo config version that getCurrentAppVersion reads.
function setCurrentVersion(version: string) {
  (Constants as any).expoConfig = { version };
}

afterEach(() => {
  global.fetch = originalFetch;
  jest.clearAllMocks();
});

describe('getCurrentAppVersion', () => {
  it('reports the Expo config version', () => {
    setCurrentVersion('1.2.3');
    expect(getCurrentAppVersion()).toBe('1.2.3');
  });

  it('falls back to 0.0.0 when no version is available', () => {
    (Constants as any).expoConfig = null;
    expect(getCurrentAppVersion()).toBe('0.0.0');
  });
});

describe('APP_SELF_UPDATE_SUPPORTED', () => {
  // The flag is computed at module load, so each case needs a fresh module
  // registry with Platform/Constants set up before app-update is evaluated.
  function loadFlagWithConfig(os: string, expoConfig: unknown): boolean {
    let flag = false;
    jest.isolateModules(() => {
      const { Platform: IsolatedPlatform } = require('react-native');
      IsolatedPlatform.OS = os;
      const IsolatedConstants = require('expo-constants').default;
      IsolatedConstants.expoConfig = expoConfig;
      flag = require('@/services/app-update').APP_SELF_UPDATE_SUPPORTED;
    });
    return flag;
  }

  function loadFlag(os: string, extra?: Record<string, unknown>): boolean {
    return loadFlagWithConfig(os, { version: '1.0.0', ...(extra !== undefined ? { extra } : {}) });
  }

  it('is enabled on Android builds without a distribution flag (dev / GitHub APK)', () => {
    expect(loadFlag('android')).toBe(true);
  });

  it('is disabled on Android Play builds (extra.distribution === "play")', () => {
    expect(loadFlag('android', { distribution: 'play' })).toBe(false);
  });

  it('is enabled on Android for other distribution values', () => {
    expect(loadFlag('android', { distribution: 'github' })).toBe(true);
  });

  it('is disabled on iOS regardless of distribution', () => {
    expect(loadFlag('ios')).toBe(false);
  });

  it('fails closed (disabled) when the Expo config is unavailable', () => {
    expect(loadFlagWithConfig('android', null)).toBe(false);
  });
});

describe('checkForAppUpdate', () => {
  it('returns update info when a newer app release exists', async () => {
    setCurrentVersion('1.0.0');
    mockReleases([appRelease('app-v1.1.0')]);

    const info = await checkForAppUpdate();
    expect(info).not.toBeNull();
    expect(info!.version).toBe('1.1.0');
    expect(info!.apkAsset?.name).toBe('rgb-sunglasses-1.1.0.apk');
    expect(info!.htmlUrl).toContain('app-v1.1.0');
  });

  it('returns null when already on the latest version', async () => {
    setCurrentVersion('1.1.0');
    mockReleases([appRelease('app-v1.1.0')]);
    expect(await checkForAppUpdate()).toBeNull();
  });

  it('returns null when the installed version is ahead', async () => {
    setCurrentVersion('2.0.0');
    mockReleases([appRelease('app-v1.1.0')]);
    expect(await checkForAppUpdate()).toBeNull();
  });

  it('returns null when no app release exists', async () => {
    setCurrentVersion('1.0.0');
    mockReleases([]);
    expect(await checkForAppUpdate()).toBeNull();
  });

  it('reports an update even if the release is missing an apk asset', async () => {
    setCurrentVersion('1.0.0');
    mockReleases([appRelease('app-v1.1.0', { apk: false })]);

    const info = await checkForAppUpdate();
    expect(info).not.toBeNull();
    expect(info!.apkAsset).toBeNull();
  });
});

describe('downloadApk', () => {
  it('downloads to the cache dir and returns the local uri', async () => {
    const asset = appRelease('app-v1.1.0').assets[0];
    const uri = await downloadApk(asset);
    expect(LegacyFS.createDownloadResumable).toHaveBeenCalled();
    expect(uri).toBe('file:///cache/firmware-update.zip'); // from the shared mock
  });
});

describe('installApk', () => {
  const originalOS = Platform.OS;
  beforeAll(() => {
    (Platform as any).OS = 'android';
  });
  afterAll(() => {
    (Platform as any).OS = originalOS;
  });

  it('converts to a content uri and launches the installer intent', async () => {
    await installApk('file:///cache/app-update.apk');
    expect(LegacyFS.getContentUriAsync).toHaveBeenCalledWith('file:///cache/app-update.apk');
    expect(IntentLauncher.startActivityAsync).toHaveBeenCalledWith(
      'android.intent.action.INSTALL_PACKAGE',
      expect.objectContaining({
        data: 'content://mock/file:///cache/app-update.apk',
        flags: 1,
        type: 'application/vnd.android.package-archive',
      })
    );
  });
});
