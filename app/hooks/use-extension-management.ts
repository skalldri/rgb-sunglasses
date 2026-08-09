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
import { DeviceFileEntry, SmpCommandError } from '@/services/mcumgr';
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
    /** Upload one release extension (install, update and repair are all this). */
    installOne: (entry: ExtensionSyncEntry) => Promise<boolean>;
    /** Delete one device file by bare name via FILE_MGMT. */
    removeOne: (name: string) => Promise<boolean>;
    /** OS-group reset — the manage → restart → re-list loop's restart. */
    reboot: () => Promise<void>;
}

const EMPTY_PLAN: ExtensionManagementPlan = { released: [], unmanaged: [], listAvailable: false };

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
    releaseAssets: GitHubAsset[]
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
                releaseAssets.length > 0 ? await planExtensionSync(client, releaseAssets) : [];

            let deviceFiles: DeviceFileEntry[] | null;
            try {
                deviceFiles = await client.listDeviceFiles('ext');
            } catch (e: unknown) {
                // A group-less error (bare rc, typically ENOTSUP) is old firmware
                // without the group: expected, not a failure. An error carrying a
                // group — 64 or otherwise — is a real device-side refusal.
                if (e instanceof SmpCommandError && e.group === undefined) {
                    deviceFiles = null;
                } else {
                    throw e;
                }
            }

            setPlan(planExtensionManagement(syncEntries, deviceFiles));
            setState('ready');
        } catch (e: unknown) {
            setError(e instanceof Error ? e.message : String(e));
            setState('error');
        }
    }, [client, releaseAssets]);

    const installOne = useCallback(
        async (entry: ExtensionSyncEntry): Promise<boolean> => {
            if (!client) return false;
            setBusyName(entry.name);
            setProgress(null);
            try {
                // Single-entry reuse of the sync pipeline: download, digest-verify,
                // upload. Deliberately no bulk path anywhere — install is always a
                // per-extension user choice (design §6).
                await syncExtensions(client, [entry], downloadExtensionAsset, setProgress);
                setMutated(true);
                await refresh();
                return true;
            } catch (e: unknown) {
                setError(e instanceof Error ? e.message : String(e));
                // Keep the plan on screen: a failed upload leaves the previous
                // listing valid, and the row's own action is the retry affordance.
                await refresh().catch(() => undefined);
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
        async (name: string): Promise<boolean> => {
            if (!client) return false;
            setBusyName(name);
            try {
                await client.deleteDeviceFile(name, 'ext');
                setMutated(true);
                await refresh();
                return true;
            } catch (e: unknown) {
                setError(e instanceof Error ? e.message : String(e));
                await refresh().catch(() => undefined);
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
