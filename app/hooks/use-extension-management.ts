import {
    ExtensionManagementPlan,
    planExtensionManagement,
} from '@/services/extension-management';
import {
    downloadExtensionAsset,
    ExtensionSyncEntry,
    ExtensionSyncProgress,
    planExtensionSync,
    syncExtensions,
} from '@/services/extension-sync';
import { GitHubAsset } from '@/services/github-releases';
import { DeviceFileEntry, FileMgmtError, SmpCommandError, SmpGroup } from '@/services/mcumgr';
import { useMcuMgrClientContext } from '@/context/mcumgr-client-context';
import { useCallback, useEffect, useState } from 'react';

export type ExtensionManagementState = 'idle' | 'checking' | 'ready' | 'error';

export interface ExtensionManagementController {
    state: ExtensionManagementState;
    plan: ExtensionManagementPlan;
    error: string;
    /** File currently being installed/updated/removed, or null. */
    busyName: string | null;
    /** Upload progress for the file being installed, when one is. */
    progress: ExtensionSyncProgress | null;
    /**
     * True once any mutation succeeded this connection — extension changes are
     * boot-scoped, so this is what makes the "Restart glasses now" offer appear.
     */
    mutated: boolean;
    refresh: () => Promise<void>;
    /**
     * Upload one release extension (install, update and repair are all this).
     * `skipRefresh` lets a batch caller (the guided flow's apply loop) re-plan
     * ONCE after the whole batch instead of after every item — a per-item
     * re-plan is a full hash sweep + LIST over serialized SMP, so N items
     * would cost O(N²) BLE round trips in the pre-restart window.
     */
    installOne: (entry: ExtensionSyncEntry, options?: { skipRefresh?: boolean }) => Promise<boolean>;
    /** Delete one device file by bare name via FILE_MGMT. Same `skipRefresh` contract. */
    removeOne: (name: string, options?: { skipRefresh?: boolean }) => Promise<boolean>;
    /** OS-group reset — the manage → restart → re-list loop's restart. */
    reboot: () => Promise<void>;
}

const EMPTY_PLAN: ExtensionManagementPlan = {
    released: [],
    unmanaged: [],
    listAvailable: false,
    releaseKnown: false,
};

/**
 * State and actions for the extension-management screen and the guided flow's
 * picker: the release-side digest plan joined with the device's FILE_MGMT LIST.
 *
 * Graceful degradation is per-source: firmware without the FILE_MGMT group
 * answers LIST with a group-less SMP error, which sets `plan.listAvailable`
 * false (hiding list/remove affordances) WITHOUT failing the release side —
 * per-row install is plain fs_mgmt upload and still works there.
 */
export function useExtensionManagement(
    releaseAssets: GitHubAsset[],
    /**
     * True only when the release lookup actually SUCCEEDED. An empty asset
     * list with this false means "unknown", not "the release ships nothing" —
     * and no removal may ever be suggested against an unknown release (a
     * failed GitHub lookup would otherwise flag every installed extension).
     */
    releaseKnown: boolean
): ExtensionManagementController {
    const { client } = useMcuMgrClientContext();

    const [state, setState] = useState<ExtensionManagementState>('idle');
    const [plan, setPlan] = useState<ExtensionManagementPlan>(EMPTY_PLAN);
    const [error, setError] = useState('');
    const [busyName, setBusyName] = useState<string | null>(null);
    const [progress, setProgress] = useState<ExtensionSyncProgress | null>(null);
    const [mutated, setMutated] = useState(false);

    const refresh = useCallback(async () => {
        if (!client) return;

        setState('checking');
        setError('');
        try {
            const syncEntries =
                releaseKnown && releaseAssets.length > 0
                    ? await planExtensionSync(client, releaseAssets)
                    : [];

            let deviceFiles: DeviceFileEntry[] | null = null;
            let listError: string | null = null;
            try {
                deviceFiles = await client.listDeviceFiles('ext');
            } catch (e: unknown) {
                // A group-less error (bare rc, typically ENOTSUP) is old firmware
                // without the group: expected, not a failure. Anything else (a
                // group-64 refusal, a transport timeout) is a real error — but it
                // must not discard the release plan just computed: per-row install
                // is plain fs_mgmt upload and still works without LIST.
                if (!(e instanceof SmpCommandError && e.group === undefined)) {
                    listError = e instanceof Error ? e.message : String(e);
                }
            }

            setPlan(planExtensionManagement(syncEntries, deviceFiles, releaseKnown));
            if (listError !== null) {
                setError(listError);
                setState('error');
            } else {
                setState('ready');
            }
        } catch (e: unknown) {
            setError(e instanceof Error ? e.message : String(e));
            setState('error');
        }
    }, [client, releaseAssets, releaseKnown]);

    const installOne = useCallback(
        async (
            entry: ExtensionSyncEntry,
            options?: { skipRefresh?: boolean }
        ): Promise<boolean> => {
            if (!client) return false;
            setBusyName(entry.name);
            setProgress(null);
            try {
                // Single-entry reuse of the sync pipeline: download, digest-verify,
                // upload. Deliberately no bulk path anywhere — install is always a
                // per-extension user choice (design §6).
                await syncExtensions(client, [entry], downloadExtensionAsset, setProgress);
                setMutated(true);
                if (!options?.skipRefresh) {
                    await refresh();
                }
                return true;
            } catch (e: unknown) {
                const message = e instanceof Error ? e.message : String(e);
                // Keep the plan on screen: a failed upload leaves the previous
                // listing valid, and the row's own action is the retry affordance.
                // The error is (re)set AFTER the refresh — refresh() clears it on
                // entry, and a blanked message renders as an empty red box.
                if (!options?.skipRefresh) {
                    await refresh();
                }
                setError(message);
                setState('error');
                return false;
            } finally {
                setBusyName(null);
                setProgress(null);
            }
        },
        [client, refresh]
    );

    const removeOne = useCallback(
        async (name: string, options?: { skipRefresh?: boolean }): Promise<boolean> => {
            if (!client) return false;
            setBusyName(name);
            try {
                try {
                    await client.deleteDeviceFile(name, 'ext');
                } catch (e: unknown) {
                    // NOT_FOUND means the file is already gone — most commonly the
                    // retry after a delete whose response timed out even though the
                    // unlink completed. The desired end state holds either way, so
                    // treat it as success (idempotent delete).
                    if (
                        !(
                            e instanceof SmpCommandError &&
                            e.group === SmpGroup.FILE_MGMT &&
                            e.rc === FileMgmtError.NOT_FOUND
                        )
                    ) {
                        throw e;
                    }
                }
                setMutated(true);
                if (!options?.skipRefresh) {
                    await refresh();
                }
                return true;
            } catch (e: unknown) {
                const message = e instanceof Error ? e.message : String(e);
                // Same ordering rule as installOne: refresh first, then set the
                // error, so refresh()'s own setError('') can't blank the message.
                if (!options?.skipRefresh) {
                    await refresh();
                }
                setError(message);
                setState('error');
                return false;
            } finally {
                setBusyName(null);
            }
        },
        [client, refresh]
    );

    const reboot = useCallback(async () => {
        if (!client) return;
        await client.reset();
    }, [client]);

    // One check per (connection, release) pair. `client` and `releaseAssets` are
    // context-stable; nothing this hook writes feeds back into either, so this
    // effect cannot re-trigger itself (the BLE-read-loop rule in app/CLAUDE.md).
    useEffect(() => {
        refresh();
    }, [refresh]);

    // Reset when the connection goes away so a reconnect re-derives everything
    // for the new boot (slot numbers and loaded flags all change at boot).
    useEffect(() => {
        if (client) return;
        setState('idle');
        setPlan(EMPTY_PLAN);
        setError('');
        setBusyName(null);
        setProgress(null);
        setMutated(false);
    }, [client]);

    return {
        state,
        plan,
        error,
        busyName,
        progress,
        mutated,
        refresh,
        installOne,
        removeOne,
        reboot,
    };
}
