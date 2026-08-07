import { useMcuMgrClientContext } from '@/context/mcumgr-client-context';
import {
    compareVersions,
    extractBoardRevision,
    fetchLatestFirmwareRelease,
    findAssetForBoard,
    GitHubAsset,
    parseVersionFromTag,
} from '@/services/github-releases';
import { ImageSlot } from '@/services/mcumgr';
import { useCallback, useEffect, useState } from 'react';

export type UpdateCheckState = 'idle' | 'checking' | 'upToDate' | 'updateAvailable' | 'error';

export interface FirmwareReleaseInfo {
    /** Board revision read from the device, e.g. 'proto0'. */
    boardRevision: string | null;
    boardDetectionError: string;
    updateCheckState: UpdateCheckState;
    /** The firmware `.zip` asset for this board. */
    latestAsset: GitHubAsset | null;
    latestVersion: string;
    /** Every asset on the release — extension sync needs the bare `.llext` ones. */
    releaseAssets: GitHubAsset[];
    updateCheckError: string;
    /** Version string of the image currently running (slot 0, active). */
    deviceVersion: string;
    /** Current image state, also used to seed the version comparison. */
    imageState: ImageSlot[];
    refreshImageState: () => Promise<void>;
}

/**
 * Board detection plus the GitHub firmware-release check.
 *
 * Lifted verbatim in behaviour from the old single-screen modal so the landing page
 * and the extension screen can both use it without either owning the other's state.
 */
export function useFirmwareRelease(): FirmwareReleaseInfo {
    const { client } = useMcuMgrClientContext();

    const [imageState, setImageState] = useState<ImageSlot[]>([]);
    const [boardRevision, setBoardRevision] = useState<string | null>(null);
    const [boardDetectionError, setBoardDetectionError] = useState<string>('');
    const [updateCheckState, setUpdateCheckState] = useState<UpdateCheckState>('idle');
    const [latestAsset, setLatestAsset] = useState<GitHubAsset | null>(null);
    const [latestVersion, setLatestVersion] = useState<string>('');
    const [releaseAssets, setReleaseAssets] = useState<GitHubAsset[]>([]);
    const [updateCheckError, setUpdateCheckError] = useState<string>('');

    const refreshImageState = useCallback(async () => {
        if (!client) return;
        try {
            const state = await client.getImageState();
            setImageState(state.images);
        } catch (e: unknown) {
            console.log('Image state unavailable:', e);
        }
    }, [client]);

    // Read image state and detect the board once a client is available. Sequenced,
    // because McuMgrClient serialises requests anyway and interleaving buys nothing.
    useEffect(() => {
        if (!client) return;
        let cancelled = false;

        async function detect() {
            await refreshImageState();
            if (cancelled) return;
            try {
                const boardName = await client!.getOsInfo('i');
                if (cancelled) return;
                const revision = extractBoardRevision(boardName);
                if (revision) {
                    setBoardRevision(revision);
                } else {
                    setBoardDetectionError(`Unknown board: ${boardName}`);
                }
            } catch (e: unknown) {
                if (cancelled) return;
                setBoardDetectionError(
                    `Board detection failed: ${e instanceof Error ? e.message : String(e)}`
                );
            }
        }

        detect();
        return () => {
            cancelled = true;
        };
    }, [client, refreshImageState]);

    // Reset when the device goes away, so a reconnect re-derives rather than showing
    // a stale "Update Available" card with versions from the previous session.
    useEffect(() => {
        if (client) return;
        setBoardRevision(null);
        setBoardDetectionError('');
        setUpdateCheckState('idle');
        setLatestAsset(null);
        setLatestVersion('');
        setReleaseAssets([]);
        setUpdateCheckError('');
    }, [client]);

    const activeSlot = imageState.find(s => s.active && s.slot === 0);
    const deviceVersion = activeSlot?.version ?? '';

    useEffect(() => {
        if (!boardRevision || updateCheckState !== 'idle') return;

        async function checkForUpdates() {
            setUpdateCheckState('checking');
            try {
                const release = await fetchLatestFirmwareRelease('skalldri', 'rgb-sunglasses');
                const asset = findAssetForBoard(release.assets, boardRevision!);
                if (!asset) {
                    throw new Error(`No firmware asset found for board: ${boardRevision}`);
                }

                const githubVersion = parseVersionFromTag(release.tag_name);
                const cmp = deviceVersion ? compareVersions(deviceVersion, githubVersion) : -1;

                setLatestAsset(asset);
                setLatestVersion(githubVersion);
                // Kept whole (not just the board's zip) so the extension check can find
                // this release's bare .llext assets.
                setReleaseAssets(release.assets);
                setUpdateCheckState(cmp < 0 ? 'updateAvailable' : 'upToDate');
            } catch (e: unknown) {
                setUpdateCheckError(e instanceof Error ? e.message : String(e));
                setUpdateCheckState('error');
            }
        }

        checkForUpdates();
        // updateCheckState is read only as a run-once idle guard, not to compute the
        // result; it's deliberately not a dependency so the check fires once per
        // board/image change rather than re-firing on its own state transitions.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [boardRevision, deviceVersion]);

    return {
        boardRevision,
        boardDetectionError,
        updateCheckState,
        latestAsset,
        latestVersion,
        releaseAssets,
        updateCheckError,
        deviceVersion,
        imageState,
        refreshImageState,
    };
}
