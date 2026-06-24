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
}

// ============================================================================
// API
// ============================================================================

export async function fetchLatestRelease(owner: string, repo: string): Promise<GitHubRelease> {
    const url = `https://api.github.com/repos/${owner}/${repo}/releases/latest`;
    const response = await fetch(url, {
        headers: {
            Accept: 'application/vnd.github+json',
            'X-GitHub-Api-Version': '2022-11-28',
        },
    });

    if (!response.ok) {
        throw new Error(`GitHub API error: ${response.status} ${response.statusText}`);
    }

    return response.json() as Promise<GitHubRelease>;
}

// ============================================================================
// Utilities
// ============================================================================

/** Strip leading "v" from a GitHub tag. "v1.2.3" → "1.2.3" */
export function parseVersionFromTag(tag: string): string {
    return tag.startsWith('v') ? tag.slice(1) : tag;
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
