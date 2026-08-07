import { useBluetooth } from '@/context/bluetooth-context';
import { useMcuMgrClient } from '@/hooks/use-mcumgr-client';
import { McuMgrClient } from '@/services/mcumgr';
import React, { createContext, useContext, useEffect } from 'react';

interface McuMgrClientContextValue {
    client: McuMgrClient | null;
    isInitializing: boolean;
    /**
     * Why there is no client, when there isn't one: either `'No device connected'`
     * or an initialisation failure.
     *
     * Treat this as **status, not an error to shout about**. A device that is
     * rebooting mid-update produces exactly this string, and the old single-screen
     * modal rendered it as red danger text — which is how a perfectly normal
     * firmware reboot came to look like a fault. Screens should render it neutrally,
     * and suppress it entirely when they are the ones that asked for the reboot.
     */
    error: string;
}

const McuMgrClientContext = createContext<McuMgrClientContextValue | null>(null);

/**
 * Owns the one and only `McuMgrClient` for the firmware-update screens.
 *
 * This exists because `useMcuMgrClient` constructs a client per call, and
 * `McuMgrClient.initialize()` registers a `monitor()` on the SMP characteristic.
 * Pushing a screen in a stack does **not** unmount the screen underneath it, so
 * calling the hook from both a landing page and a pushed flow page would mean two
 * live clients on one characteristic: two notification registrations against
 * Android's 15-slot budget, and two response handlers racing for the same replies.
 * Mounting this once in the route-group layout makes that structurally impossible.
 *
 * It also owns the `selectedDevice.mcuMgrClient` patch, so the Bluetooth context's
 * disconnect cleanup still has a client to tear down and exactly one place writes it.
 */
export function McuMgrClientProvider({ children }: { children: React.ReactNode }) {
    const { selectedDevice, setSelectedDevice } = useBluetooth();
    const { client, isInitializing, error } = useMcuMgrClient(selectedDevice?.device ?? null);

    useEffect(() => {
        if (client && selectedDevice) {
            setSelectedDevice({ ...selectedDevice, mcuMgrClient: client });
        }
        // Only re-run when the client or device MAC changes: depending on the full
        // `selectedDevice` object would loop forever, since this effect itself calls
        // setSelectedDevice with a new object. (setSelectedDevice is a stable context setter.)
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [client, selectedDevice?.mac]);

    return (
        <McuMgrClientContext.Provider value={{ client, isInitializing, error }}>
            {children}
        </McuMgrClientContext.Provider>
    );
}

/**
 * The shared MCUmgr client for the current device.
 *
 * Returns a null client (rather than throwing) when used outside the provider, so a
 * screen rendered in isolation by a unit test behaves like a disconnected device
 * instead of crashing.
 */
export function useMcuMgrClientContext(): McuMgrClientContextValue {
    return (
        useContext(McuMgrClientContext) ?? {
            client: null,
            isInitializing: false,
            error: 'No device connected',
        }
    );
}
