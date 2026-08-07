import { useFirmwareReleaseLookup, type FirmwareReleaseInfo } from '@/hooks/use-firmware-release';
import React, { createContext, useCallback, useContext, useMemo, useRef, useState } from 'react';

interface FirmwareUpdateContextValue {
    release: FirmwareReleaseInfo;
    /**
     * True while a firmware transfer or restart is in flight on the flow screen.
     *
     * The screens are stacked, not exclusive — a pushed screen leaves the ones below
     * it mounted and interactive — so "an upload is running" has to be shared state.
     * Without it the debug page's Reset Device / Erase Slot 1 stay enabled during an
     * upload, and a reset landing between chunks reboots the device mid-transfer,
     * leaving a partial image in slot 1. The old single-screen modal got this for free
     * with `disabled={isUploading}`; splitting the screens is what took it away.
     */
    isBusy: boolean;
    setBusy: (busy: boolean) => void;
}

const FirmwareUpdateContext = createContext<FirmwareUpdateContextValue | null>(null);

/**
 * Shared state for the firmware-update screens: one release lookup, one busy flag.
 *
 * The release lookup lives here because every screen wants it and pushed screens never
 * unmount — a per-screen `useFirmwareRelease()` meant the landing page, the extensions
 * screen and the flow each fired their own unauthenticated GitHub fetch, stacking up
 * against the 60 req/hr per-IP cap that `app/CLAUDE.md` warns not to multiply, plus
 * duplicate `getOsInfo`/`getImageState` SMP traffic on every navigation.
 */
export function FirmwareUpdateProvider({ children }: { children: React.ReactNode }) {
    const release = useFirmwareReleaseLookup();
    const [isBusy, setIsBusy] = useState(false);
    // Guards against a screen unmounting mid-flight and leaving the flag stuck on.
    const busyOwners = useRef(0);

    const setBusy = useCallback((busy: boolean) => {
        busyOwners.current = Math.max(0, busyOwners.current + (busy ? 1 : -1));
        setIsBusy(busyOwners.current > 0);
    }, []);

    const value = useMemo(() => ({ release, isBusy, setBusy }), [release, isBusy, setBusy]);

    return (
        <FirmwareUpdateContext.Provider value={value}>{children}</FirmwareUpdateContext.Provider>
    );
}

/**
 * The shared release lookup. Falls back to an inert value outside the provider so a
 * screen rendered in isolation by a unit test behaves like "nothing found yet" rather
 * than crashing.
 */
export function useFirmwareRelease(): FirmwareReleaseInfo {
    return (
        useContext(FirmwareUpdateContext)?.release ?? {
            boardRevision: null,
            boardDetectionError: '',
            updateCheckState: 'idle',
            latestAsset: null,
            latestVersion: '',
            releaseAssets: [],
            updateCheckError: '',
            deviceVersion: '',
            imageState: [],
            refreshImageState: async () => undefined,
        }
    );
}

/** Whether a transfer/restart is in flight, and the setter the flow uses to claim it. */
export function useFirmwareBusy(): { isBusy: boolean; setBusy: (busy: boolean) => void } {
    const ctx = useContext(FirmwareUpdateContext);
    return { isBusy: ctx?.isBusy ?? false, setBusy: ctx?.setBusy ?? (() => undefined) };
}
