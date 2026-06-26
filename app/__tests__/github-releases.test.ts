import {
  compareVersions,
  extractBoardRevision,
  fetchLatestFirmwareRelease,
  findAssetForBoard,
  GitHubAsset,
  GitHubRelease,
  parseVersionFromTag,
} from '@/services/github-releases';

function release(tag: string, assetNames: string[], extra: Partial<GitHubRelease> = {}): GitHubRelease {
  return {
    id: tag.length,
    tag_name: tag,
    name: tag,
    published_at: '2026-01-01T00:00:00Z',
    assets: assetNames.map(
      (name, i): GitHubAsset => ({
        id: i,
        name,
        browser_download_url: `https://example.com/${name}`,
        size: 1,
        content_type: 'application/zip',
      })
    ),
    ...extra,
  };
}

const originalFetch = global.fetch;

afterEach(() => {
  global.fetch = originalFetch;
  jest.restoreAllMocks();
});

function mockFetchReleases(releases: GitHubRelease[]) {
  // Single page: any short page (< 100) ends pagination after one request.
  mockFetchPages([releases]);
}

/** Mock the releases list endpoint to return successive pages, then empty. */
function mockFetchPages(pages: GitHubRelease[][]) {
  let call = 0;
  global.fetch = jest.fn(async () => {
    const body = pages[call] ?? [];
    call++;
    return {
      ok: true,
      status: 200,
      statusText: 'OK',
      json: async () => body,
    };
  }) as unknown as typeof fetch;
}

describe('parseVersionFromTag', () => {
  it.each([
    ['fw-v1.2.3', '1.2.3'],
    ['app-v1.2.3', '1.2.3'],
    ['v1.2.3', '1.2.3'],
    ['1.2.3', '1.2.3'],
    ['fw-v1.2.3+build7', '1.2.3+build7'],
  ])('%s -> %s', (tag, expected) => {
    expect(parseVersionFromTag(tag)).toBe(expected);
  });
});

describe('fetchLatestFirmwareRelease', () => {
  it('ignores app-v releases and returns the firmware one (the reported bug)', async () => {
    // app-v published most recently — /releases/latest would have returned this.
    mockFetchReleases([
      release('app-v1.0.0', ['rgb-sunglasses-1.0.0.apk', 'rgb-sunglasses-1.0.0-qr.png']),
      release('fw-v1.0.0', ['dfu_application_proto0.zip', 'dfu_application_dk.zip']),
    ]);

    const result = await fetchLatestFirmwareRelease('skalldri', 'rgb-sunglasses');
    expect(result.tag_name).toBe('fw-v1.0.0');
    expect(findAssetForBoard(result.assets, 'proto0')?.name).toBe('dfu_application_proto0.zip');
  });

  it('picks the highest firmware version regardless of list order', async () => {
    mockFetchReleases([
      release('fw-v1.0.9', ['dfu_application_proto0.zip']),
      release('fw-v1.0.10', ['dfu_application_proto0.zip']),
      release('app-v2.0.0', ['rgb-sunglasses-2.0.0.apk']),
    ]);

    const result = await fetchLatestFirmwareRelease('skalldri', 'rgb-sunglasses');
    expect(result.tag_name).toBe('fw-v1.0.10');
  });

  it('pages past the first 100 results to find a later firmware release', async () => {
    // A full first page of app releases pushes the firmware release onto page 2;
    // /releases/latest-style single-page fetching would miss it entirely.
    const page1 = Array.from({ length: 100 }, (_, i) => release(`app-v0.0.${i}`, ['app.apk']));
    const page2 = [release('fw-v1.0.0', ['dfu_application_proto0.zip'])];
    mockFetchPages([page1, page2]);

    const result = await fetchLatestFirmwareRelease('skalldri', 'rgb-sunglasses');
    expect(result.tag_name).toBe('fw-v1.0.0');
    expect(global.fetch).toHaveBeenCalledTimes(2);
  });

  it('ignores firmware tags whose version is unparseable', async () => {
    mockFetchReleases([
      release('fw-vnightly', ['dfu_application_proto0.zip']),
      release('fw-v1.0.0', ['dfu_application_proto0.zip']),
    ]);

    const result = await fetchLatestFirmwareRelease('skalldri', 'rgb-sunglasses');
    expect(result.tag_name).toBe('fw-v1.0.0');
  });

  it('skips draft and prerelease firmware releases', async () => {
    mockFetchReleases([
      release('fw-v2.0.0', ['dfu_application_proto0.zip'], { draft: true }),
      release('fw-v1.5.0', ['dfu_application_proto0.zip'], { prerelease: true }),
      release('fw-v1.0.0', ['dfu_application_proto0.zip']),
    ]);

    const result = await fetchLatestFirmwareRelease('skalldri', 'rgb-sunglasses');
    expect(result.tag_name).toBe('fw-v1.0.0');
  });

  it('throws a clear error when there are no firmware releases', async () => {
    mockFetchReleases([release('app-v1.0.0', ['rgb-sunglasses-1.0.0.apk'])]);
    await expect(fetchLatestFirmwareRelease('skalldri', 'rgb-sunglasses')).rejects.toThrow(
      'No firmware release found'
    );
  });
});

describe('findAssetForBoard / extractBoardRevision / compareVersions (unchanged behaviour)', () => {
  it('matches the board-specific .zip', () => {
    const assets = release('fw-v1.0.0', ['dfu_application_proto0.zip', 'dfu_application_dk.zip']).assets;
    expect(findAssetForBoard(assets, 'proto0')?.name).toBe('dfu_application_proto0.zip');
    expect(findAssetForBoard(assets, 'dk')?.name).toBe('dfu_application_dk.zip');
  });

  it('derives the board revision from the OS board name', () => {
    expect(extractBoardRevision('rgb_sunglasses_proto0_nrf5340_cpuapp')).toBe('proto0');
    expect(extractBoardRevision('rgb_sunglasses_dk_nrf5340_cpuapp')).toBe('dk');
  });

  it('orders versions correctly', () => {
    expect(compareVersions('1.0.0', '2.0.0')).toBe(-1);
    expect(compareVersions('1.0.10', '1.0.9')).toBe(1);
  });
});
