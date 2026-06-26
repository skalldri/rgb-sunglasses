// ============================================================================
// Types
// ============================================================================

export interface GitHubAsset {
    id: number;
    name: string;
    browser_download_url: string;
    size: number;
    content_type: string;
}

export interface GitHubRelease {
    id: number;
    tag_name: string;
    name: string;
    published_at: string;
    assets: GitHubAsset[];
    // Present on the list endpoint (/releases); absent/undefined is treated as "not a draft".
    draft?: boolean;
    prerelease?: boolean;
}

// ============================================================================
// API
// ============================================================================

/**
 * Tag prefix identifying firmware releases. This is a monorepo that publishes
 * both firmware (`fw-v*`) and companion-app (`app-v*`) releases — see the two
 * release workflows under `.github/workflows/`.
 */
export const FIRMWARE_TAG_PREFIX = 'fw-v';

// Page size for the releases list, and a hard cap on pages so a misbehaving API
// (or an unexpectedly huge history) can't spin unbounded requests against the
// unauthenticated 60-req/hr rate limit.
const RELEASES_PER_PAGE = 100;
const MAX_RELEASE_PAGES = 10;

/**
 * Fetch the repo's releases, newest first (includes drafts/prereleases).
 *
 * Pages through the list endpoint until a short (or empty) page is returned, so
 * a firmware (`fw-v*`) release isn't missed just because enough app (`app-v*`)
 * releases were published after it to push it past the first page.
 */
export async function fetchReleases(owner: string, repo: string): Promise<GitHubRelease[]> {
    const all: GitHubRelease[] = [];

    for (let page = 1; page <= MAX_RELEASE_PAGES; page++) {
        const url = `https://api.github.com/repos/${owner}/${repo}/releases?per_page=${RELEASES_PER_PAGE}&page=${page}`;
        const response = await fetch(url, {
            headers: {
                Accept: 'application/vnd.github+json',
                'X-GitHub-Api-Version': '2022-11-28',
            },
        });

        if (!response.ok) {
            throw new Error(`GitHub API error: ${response.status} ${response.statusText}`);
        }

        const batch = (await response.json()) as GitHubRelease[];
        all.push(...batch);

        // A page shorter than the page size means we've reached the end.
        if (batch.length < RELEASES_PER_PAGE) break;
    }

    return all;
}

/**
 * Fetch the latest *firmware* release.
 *
 * We can't use GitHub's `/releases/latest` here: it returns the single newest
 * release of any kind, which in this monorepo can be an `app-v*` release that
 * carries no firmware `.zip` asset (this caused "No firmware asset found for
 * board: <rev>"). Instead, list all releases, keep only published firmware ones
 * (`fw-v*`), and pick the highest version.
 */
export async function fetchLatestFirmwareRelease(owner: string, repo: string): Promise<GitHubRelease> {
    const releases = await fetchReleases(owner, repo);
    const firmwareReleases = releases.filter(
        r =>
            r.tag_name.startsWith(FIRMWARE_TAG_PREFIX) &&
            !r.draft &&
            !r.prerelease &&
            // Skip tags whose version we can't parse — otherwise compareVersions()
            // sees NaN parts, the sort comparator returns 0, and "latest" silently
            // degrades to input order.
            isParseableVersionTag(r.tag_name)
    );

    if (firmwareReleases.length === 0) {
        throw new Error('No firmware release found');
    }

    // GitHub returns releases by creation order, which isn't guaranteed to track
    // semver — sort explicitly so e.g. a fw-v1.0.10 patch beats fw-v1.0.9.
    firmwareReleases.sort((a, b) =>
        compareVersions(parseVersionFromTag(b.tag_name), parseVersionFromTag(a.tag_name))
    );

    return firmwareReleases[0];
}

// ============================================================================
// Utilities
// ============================================================================

// Matches a "major.minor.patch[+build]" version anywhere in a tag string.
const VERSION_RE = /(\d+\.\d+\.\d+(?:\+[0-9A-Za-z.-]+)?)/;

/**
 * Extract the semantic version from a GitHub tag, ignoring any release-channel
 * prefix. Handles this repo's "fw-v"/"app-v" prefixes as well as a bare "v":
 * "fw-v1.2.3" → "1.2.3", "v1.2.3" → "1.2.3", "1.2.3" → "1.2.3".
 */
export function parseVersionFromTag(tag: string): string {
    const match = tag.match(VERSION_RE);
    return match ? match[1] : tag;
}

/** True if a tag contains a parseable "major.minor.patch" version. */
export function isParseableVersionTag(tag: string): boolean {
    return VERSION_RE.test(tag);
}

/**
 * Compare two version strings of the form "major.minor.revision[+build]".
 * Build metadata is ignored. Returns -1 if a < b, 0 if equal, 1 if a > b.
 */
export function compareVersions(a: string, b: string): -1 | 0 | 1 {
    const stripBuild = (v: string) => v.split('+')[0];
    const partsA = stripBuild(a).split('.').map(Number);
    const partsB = stripBuild(b).split('.').map(Number);

    for (let i = 0; i < 3; i++) {
        const pa = partsA[i] ?? 0;
        const pb = partsB[i] ?? 0;
        if (pa < pb) return -1;
        if (pa > pb) return 1;
    }
    return 0;
}

/** Find the first .zip asset whose name contains the boardRevision string. */
export function findAssetForBoard(assets: GitHubAsset[], boardRevision: string): GitHubAsset | null {
    const lower = boardRevision.toLowerCase();
    return assets.find(a => a.name.toLowerCase().includes(lower) && a.name.endsWith('.zip')) ?? null;
}

/**
 * Extract board revision from an OS info board name string.
 * "rgb_sunglasses_proto0_nrf5340_cpuapp" → "proto0"
 * "rgb_sunglasses_dk_nrf5340_cpuapp"     → "dk"
 */
export function extractBoardRevision(boardName: string): 'proto0' | 'dk' | null {
    const lower = boardName.toLowerCase();
    if (lower.includes('proto0')) return 'proto0';
    if (lower.includes('_dk_')) return 'dk';
    return null;
}
