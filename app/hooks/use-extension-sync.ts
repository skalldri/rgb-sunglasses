import { ExtensionCheckState } from '@/components/extension-sync-card';
import { useBluetooth } from '@/context/bluetooth-context';
import { useMcuMgrClientContext } from '@/context/mcumgr-client-context';
import {
    countDeviceExtensions,
    countUnmanagedExtensions,
    downloadExtensionAsset,
    entriesNeedingUpload,
    ExtensionSyncEntry,
    ExtensionSyncProgress,
    planExtensionSync,
    syncExtensions,
} from '@/services/extension-sync';
import { GitHubAsset } from '@/services/github-releases';
import { useCallback, useEffect, useRef, useState } from 'react';

export interface ExtensionSyncController {
    state: ExtensionCheckState;
    entries: ExtensionSyncEntry[];
    unmanagedCount: number;
    error: string;
    isSyncing: boolean;
    progress: ExtensionSyncProgress | null;
    /** How many extensions still need uploading — 0 means nothing to do. */
    pendingCount: number;
    sync: () => Promise<boolean>;
    refresh: () => Promise<void>;
}

/**
 * Plan and run extension sync for the current device against a release's assets.
 *
 * Lifted from the old single-screen modal so the dedicated extensions screen owns it.
 * Behaviour (including the two subtleties below) is unchanged from what shipped in #287.
 */
export function useExtensionSync(releaseAssets: GitHubAsset[]): ExtensionSyncController {
    const { client } = useMcuMgrClientContext();
    const { selectedDevice } = useBluetooth();

    const [state, setState] = useState<ExtensionCheckState>('idle');
    const [entries, setEntries] = useState<ExtensionSyncEntry[]>([]);
    const [unmanagedCount, setUnmanagedCount] = useState(0);
    const [error, setError] = useState<string>('');
    const [isSyncing, setIsSyncing] = useState(false);
    const [progress, setProgress] = useState<ExtensionSyncProgress | null>(null);
    // Latches once the unmanaged count has been derived for this connection.
    const unmanagedComputedRef = useRef(false);

    const refresh = useCallback(async () => {
        if (!client || releaseAssets.length === 0) return;

        setState('checking');
        setError('');
        try {
            const plan = await planExtensionSync(client, releaseAssets);
            setEntries(plan);

            // Only ever computed from the FIRST plan of a connection. The device
            // extension count comes from GATT services, which don't change until the
            // firmware re-scans at boot - so comparing it against a post-sync plan
            // would count just-uploaded files as loaded and silently drop the warning
            // to zero while the stale extensions are still installed.
            if (!unmanagedComputedRef.current) {
                const deviceExtensions = countDeviceExtensions(
                    Object.keys(selectedDevice?.characteristicsByService ?? {})
                );
                setUnmanagedCount(countUnmanagedExtensions(deviceExtensions, plan));
                unmanagedComputedRef.current = true;
            }
            setState('ready');
        } catch (e: unknown) {
            // Firmware without MCUmgr file management answers every FS command with an
            // error, so this is an expected outcome on an older build - surfaced in the
            // card rather than raised as an update failure.
            setError(e instanceof Error ? e.message : String(e));
            setState('error');
        }
        // characteristicsByService is read for a one-off count at check time; it must
        // not re-trigger the check, which issues SMP traffic.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [client, releaseAssets]);

    const sync = useCallback(async (): Promise<boolean> => {
        if (!client || entriesNeedingUpload(entries).length === 0) return true;

        setIsSyncing(true);
        setProgress(null);
        try {
            await syncExtensions(client, entries, downloadExtensionAsset, setProgress);
            await refresh();
            return true;
        } catch (e: unknown) {
            setError(e instanceof Error ? e.message : String(e));
            // Re-plan before surfacing the error so the card lists what actually landed
            // before the failure rather than the pre-sync picture, and so a retry only
            // re-uploads what is still outstanding. Best-effort: if the link is gone this
            // fails too, and the stale entries still render the retry button.
            await refresh().catch(() => undefined);
            setState('error');
            return false;
        } finally {
            setIsSyncing(false);
            setProgress(null);
        }
    }, [client, entries, refresh]);

    useEffect(() => {
        refresh();
    }, [refresh]);

    // Reset on disconnect so a reconnect re-derives the unmanaged count for the new boot.
    useEffect(() => {
        if (client) return;
        setState('idle');
        setEntries([]);
        setUnmanagedCount(0);
        setError('');
        unmanagedComputedRef.current = false;
    }, [client]);

    return {
        state,
        entries,
        unmanagedCount,
        error,
        isSyncing,
        progress,
        pendingCount: entriesNeedingUpload(entries).length,
        sync,
        refresh,
    };
}
