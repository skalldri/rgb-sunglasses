import { describeWriteError } from "@/services/ble-errors";
import { McuMgrClient } from "@/services/mcumgr";
import { createContext, Dispatch, ReactNode, SetStateAction, useCallback, useContext, useMemo, useRef, useState } from "react";
import { Characteristic, Device, Service, Subscription } from "react-native-ble-plx";

export interface CharacteristicInfo {
    characteristic: Characteristic;
    value: string | null;
    name: string | null;
    cpfFormat: number | null;
    isUpdateInProgress: boolean;
    // Human-readable reason the most recent write to this characteristic failed, or null/undefined
    // when the last write succeeded (or none has been attempted). Set in the write helpers' catch,
    // cleared at the start of the next write and whenever a fresh value arrives (optimistic update
    // or device notification). Surfaced as a per-row warning icon. See issue #92.
    lastWriteError?: string | null;
}

export type BluetoothContextDevice = {
    name: string;
    mac: string;
    device: Device;
    services: Service[];
    characteristicsByService: Record<string, Record<string, CharacteristicInfo>>;
    // Flat map for efficient characteristic lookups
    characteristics: Record<string, CharacteristicInfo>;
    // Map of service UUID to array of characteristic UUIDs for rendering order
    serviceCharacteristics: Record<string, string[]>;
    // Map of service UUID to its firmware-reported display name (e.g. animation services),
    // populated during discovery from the "Animation Name" characteristic. Optional since
    // older fixtures/devices may not have it.
    serviceDisplayNames?: Record<string, string>;
    mcuMgrClient?: McuMgrClient;
};

export type DiscoveryProgress = { current: number; total: number };

// The device a connect() is currently in flight for (name + mac), or null when no connect is
// running. Lives at the context level because the Connect screen (bluetooth.tsx) builds its list
// purely from live scan advertisements, but a device stops advertising the moment its LE link
// comes up - i.e. for the whole pairing + discovery phase. Without this, the scan-derived list
// prunes the connecting device after its TTL and unmounts its in-progress row mid-pairing, so
// pairing looks like it failed (issue #158). The screen reads this to pin the connecting device
// in the list until connect() settles.
export type ConnectingDevice = { mac: string; name: string };

type BluetoothContextType = {
    selectedDevice: BluetoothContextDevice | null;
    setSelectedDevice: (device: BluetoothContextDevice | null) => void;
    isScanning: boolean;
    setIsScanning: (scanning: boolean) => void;
    // The device a connect() is currently in flight for, or null. See ConnectingDevice above.
    connectingDevice: ConnectingDevice | null;
    // Full useState dispatch type (accepts a functional updater), so a settling connect can clear
    // the pin compare-and-swap style - only if it still points at its own device - rather than
    // unconditionally nulling a slot a concurrent connect may have since overwritten.
    setConnectingDevice: Dispatch<SetStateAction<ConnectingDevice | null>>;
    // The device an automatic reconnect loop is currently running for (issue #124), or null.
    // Set when an UNEXPECTED disconnect fires the onDeviceDisconnected handler; cleared on
    // reconnect success, user cancel, or user disconnect. Drives the "Reconnecting…" row state
    // and pins the device in the Connect list (it may not be advertising reliably mid-loop).
    reconnectingDevice: ConnectingDevice | null;
    setReconnectingDevice: Dispatch<SetStateAction<ConnectingDevice | null>>;
    // Cancel token for the reconnect supervision loop (issue #124): the loop captures the value
    // at start and bails once it changes. Bumped by cancelReconnect()/disconnect(). A ref (not
    // state) so the loop - which lives in a bleManager-listener closure, possibly outliving the
    // component that created it - always reads the current value synchronously.
    reconnectGeneration: React.MutableRefObject<number>;
    // Belt-and-suspenders flag marking a user-initiated disconnect (issue #124). The primary
    // guard is structural (disconnect() removes the onDeviceDisconnected subscription before
    // cancelling), but any future code path that drops the link on purpose (e.g. an OTA-reboot
    // flow) can set this to suppress the auto-reconnect the resulting disconnect event would
    // otherwise start.
    intentionalDisconnectRef: React.MutableRefObject<boolean>;
    // In-flight connect() promise per device MAC (issue #124). Lives at context level (not in
    // useBleConnection's per-row instance) so the same-device dedup survives the row unmounting
    // and remounting - the reconnect loop and a user tapping Connect must share one attempt, or
    // overlapping connectToDevice() calls split-brain the native module (see use-ble-connection).
    connectPromises: React.MutableRefObject<Record<string, Promise<boolean>>>;
    // Characteristic-query progress during connect()'s discovery walk; null when not connecting
    // or once discovery has finished. See use-ble-connection.ts.
    discoveryProgress: DiscoveryProgress | null;
    setDiscoveryProgress: (progress: DiscoveryProgress | null) => void;
    writeToCharacteristic: (
        charUuid: string,
        newEncodedValue: string,
        options?: { skipOptimisticUpdate?: boolean; optimisticValue?: string }
    ) => Promise<boolean>;
    getCharacteristicInfo: (charUuid: string) => CharacteristicInfo | null;
    updateCharValue: (charUuid: string, newValue: string) => void;
    // Service-aware variants for characteristics whose UUID is intentionally reused across
    // multiple services (e.g. UUID_IS_ACTIVE_CHARACTERISTIC) and therefore can't be addressed by
    // bare charUuid alone. See the comment on writeServiceCharacteristic below for why these
    // exist alongside (not instead of) the bare-UUID methods above.
    getServiceCharacteristicInfo: (serviceUuid: string, charUuid: string) => CharacteristicInfo | null;
    updateServiceCharacteristicValue: (serviceUuid: string, charUuid: string, newValue: string) => void;
    writeServiceCharacteristic: (
        serviceUuid: string,
        charUuid: string,
        newEncodedValue: string,
        options?: { skipOptimisticUpdate?: boolean; optimisticValue?: string }
    ) => Promise<boolean>;
    // Monitor subscription management (persists across navigation)
    monitorSubscriptions: React.MutableRefObject<Subscription[]>;
    disconnectSubscription: React.MutableRefObject<Subscription | null>;
    // Always-live ref mirror of selectedDevice, for callbacks that outlive component
    // renders (the disconnect handler and the reconnect loop live on the bleManager
    // emitter and can run long after the row that created them unmounted - a
    // hook-local ref freezes at that row's last render and reported the OLD device
    // as still connected, silently killing the reconnect loop; hardware-observed,
    // issue #124). The provider keeps it updated every render; the disconnect paths
    // also null it SYNCHRONOUSLY (before setSelectedDevice(null) commits) so a
    // same-tick reader never sees the stale device.
    selectedDeviceRef: React.MutableRefObject<BluetoothContextDevice | null>;
};

const BluetoothContext = createContext<BluetoothContextType | undefined>(undefined);


export function BluetoothProvider({ children }: { children: ReactNode }) {
    const [selectedDevice, setSelectedDevice] = useState<BluetoothContextDevice | null>(null);
    const [isScanning, setIsScanning] = useState<boolean>(false);
    const [discoveryProgress, setDiscoveryProgress] = useState<DiscoveryProgress | null>(null);
    const [connectingDevice, setConnectingDevice] = useState<ConnectingDevice | null>(null);
    const [reconnectingDevice, setReconnectingDevice] = useState<ConnectingDevice | null>(null);
    const reconnectGeneration = useRef(0);
    const intentionalDisconnectRef = useRef(false);
    const connectPromises = useRef<Record<string, Promise<boolean>>>({});

    // Use ref to access current device in callbacks without stale closures
    const selectedDeviceRef = useRef<BluetoothContextDevice | null>(null);
    selectedDeviceRef.current = selectedDevice;

    // Store subscriptions at context level so they persist across navigation
    const monitorSubscriptions = useRef<Subscription[]>([]);
    const disconnectSubscription = useRef<Subscription | null>(null);

    // Helper to find which service contains a characteristic
    const findServiceUuidForChar = useCallback((charUuid: string): string | undefined => {
        const device = selectedDeviceRef.current;
        if (!device) return undefined;
        // Use flat map for O(1) lookup
        if (device.characteristics[charUuid]) {
            return device.serviceCharacteristics && Object.keys(device.serviceCharacteristics).find(
                svcUuid => device.serviceCharacteristics[svcUuid].includes(charUuid)
            );
        }
        return undefined;
    }, []);

    // Helper to get characteristic info by UUID
    const getCharacteristicInfo = useCallback((charUuid: string): CharacteristicInfo | null => {
        const device = selectedDeviceRef.current;
        if (!device) return null;
        // Use flat map for O(1) lookup
        return device.characteristics?.[charUuid] ?? null;
    }, []);

    // Shared helper: patches fields on a characteristic in both the flat and nested maps. `fields`
    // may be a plain patch object, or a function of the characteristic's current state that returns
    // a patch (or null to make it a no-op) — the function form runs inside the state updater, so it
    // always sees the latest committed value, which is what makes conditional/compare-and-swap
    // updates race-free.
    const updateCharFields = useCallback((
        charUuid: string,
        fields: Partial<CharacteristicInfo> | ((current: CharacteristicInfo) => Partial<CharacteristicInfo> | null)
    ) => {
        setSelectedDevice(prevDevice => {
            if (!prevDevice) return null;

            const updatedChar = prevDevice.characteristics[charUuid];
            if (!updatedChar) return prevDevice;

            const resolvedFields = typeof fields === 'function' ? fields(updatedChar) : fields;
            if (!resolvedFields) return prevDevice;

            const serviceUuid = Object.keys(prevDevice.serviceCharacteristics || {}).find(
                svc => prevDevice.serviceCharacteristics[svc].includes(charUuid)
            );
            if (!serviceUuid) return prevDevice;

            return {
                ...prevDevice,
                characteristics: {
                    ...prevDevice.characteristics,
                    [charUuid]: { ...updatedChar, ...resolvedFields }
                },
                characteristicsByService: {
                    ...prevDevice.characteristicsByService,
                    [serviceUuid]: {
                        ...prevDevice.characteristicsByService[serviceUuid],
                        [charUuid]: {
                            ...prevDevice.characteristicsByService[serviceUuid][charUuid],
                            ...resolvedFields
                        }
                    }
                }
            };
        });
    }, []);

    // Helper to update characteristic value in context. A fresh value (optimistic update or device
    // notification) also clears any stale lastWriteError - the write that failed has been
    // superseded, so the warning icon must not outlive it (issue #92, care point 1).
    const updateCharValue = useCallback((charUuid: string, newValue: string) => {
        updateCharFields(charUuid, { value: newValue, lastWriteError: null });
    }, [updateCharFields]);

    // Helper to set isUpdateInProgress flag
    const setCharUpdateInProgress = useCallback((charUuid: string, inProgress: boolean) => {
        updateCharFields(charUuid, { isUpdateInProgress: inProgress });
    }, [updateCharFields]);

    // Write to a characteristic and update context state.
    // Returns true on success, false on failure.
    //
    // skipOptimisticUpdate: most characteristics' written value IS their new stored value, so
    // assuming that locally (before any notification round-trip) is a safe, responsive default.
    // That assumption breaks for characteristics whose firmware deliberately stores something
    // different from what was written (e.g. the generic dropdown-list format: the client writes
    // just the bare selected option, but the device reorders it server-side into the full
    // "selected\nother\nother2" list and notifies that back) — applying the optimistic value
    // there clobbers the real device state with a single truncated option. Callers that know
    // their characteristic's write value isn't its canonical stored value should pass this.
    //
    // optimisticValue: the better option for that same case — instead of skipping optimism (and
    // waiting a full write+notify round-trip for any UI feedback), pass the exact canonical value
    // you know the device will store (e.g. the dropdown list locally reordered selected-first). The
    // UI updates instantly and the eventual notify just re-affirms the identical value.
    const writeToCharacteristic = useCallback(async (
        charUuid: string,
        newEncodedValue: string,
        options?: { skipOptimisticUpdate?: boolean; optimisticValue?: string }
    ): Promise<boolean> => {
        const charInfo = getCharacteristicInfo(charUuid);
        if (!charInfo) {
            console.log(`Characteristic ${charUuid} not found`);
            return false;
        }

        // Clear any prior failure as the new attempt starts (issue #92, care point a).
        updateCharFields(charUuid, { isUpdateInProgress: true, lastWriteError: null });

        // Apply the optimistic value update synchronously, BEFORE awaiting the BLE write, so it is
        // batched into the same render as isUpdateInProgress=true. A controlled input (e.g. the
        // boolean Switch) then never renders with its stale value while the async write is in
        // flight, which is what caused the on->off->on toggle flicker (issue #91). Reverted in the
        // catch below if the write is rejected.
        const previousValue = charInfo.value ?? '';
        // The value shown optimistically is usually the bytes we're writing, but a caller whose
        // written value differs from the characteristic's canonical stored value (a dropdown writes
        // a bare option; the device stores the reordered "selected\nother..." list) can supply
        // `optimisticValue` — the value it KNOWS the device will settle on — so the UI updates
        // instantly without the truncation that made these skip optimism entirely.
        const optimisticValue = options?.optimisticValue ?? newEncodedValue;
        if (!options?.skipOptimisticUpdate) {
            updateCharValue(charUuid, optimisticValue);
        }

        try {
            await charInfo.characteristic.writeWithResponse(newEncodedValue);
            return true;
        } catch (error) {
            // Revert the optimistic value — but only if it's still what we wrote. A device
            // notification (or a newer overlapping write) may have landed during the in-flight
            // write; blindly restoring previousValue would clobber that fresher state with a stale
            // one. The compare-and-swap runs inside the state updater so it sees the latest value.
            if (!options?.skipOptimisticUpdate) {
                updateCharFields(charUuid, current => current.value === optimisticValue ? { value: previousValue } : null);
            }
            console.log(`Error writing value to characteristic ${charUuid}: ${error}`);
            updateCharFields(charUuid, { lastWriteError: describeWriteError(error) });
            return false;
        } finally {
            setCharUpdateInProgress(charUuid, false);
        }
    }, [getCharacteristicInfo, setCharUpdateInProgress, updateCharValue, updateCharFields]);

    // Service-aware lookup for characteristics whose UUID is reused across services (e.g. "Is
    // Active"), where the flat characteristics map can only ever hold one (ambiguous) entry.
    const getServiceCharacteristicInfo = useCallback((serviceUuid: string, charUuid: string): CharacteristicInfo | null => {
        const device = selectedDeviceRef.current;
        if (!device) return null;
        return device.characteristicsByService?.[serviceUuid]?.[charUuid] ?? null;
    }, []);

    // Service-aware sibling of updateCharFields: patches characteristicsByService directly by the
    // given serviceUuid instead of reverse-searching serviceCharacteristics (the lookup
    // updateCharFields relies on, which is ambiguous for a charUuid reused across services). Only
    // patches the flat map too if that charUuid actually lives there (it won't, for reused-UUID
    // characteristics), so this is safe to use generically.
    const updateServiceCharacteristicFields = useCallback((
        serviceUuid: string,
        charUuid: string,
        fields: Partial<CharacteristicInfo> | ((current: CharacteristicInfo) => Partial<CharacteristicInfo> | null)
    ) => {
        setSelectedDevice(prevDevice => {
            if (!prevDevice) return null;

            const existingInService = prevDevice.characteristicsByService?.[serviceUuid]?.[charUuid];
            if (!existingInService) return prevDevice;

            const resolvedFields = typeof fields === 'function' ? fields(existingInService) : fields;
            if (!resolvedFields) return prevDevice;

            const updatedFlat = prevDevice.characteristics[charUuid]
                ? { ...prevDevice.characteristics, [charUuid]: { ...prevDevice.characteristics[charUuid], ...resolvedFields } }
                : prevDevice.characteristics;

            return {
                ...prevDevice,
                characteristics: updatedFlat,
                characteristicsByService: {
                    ...prevDevice.characteristicsByService,
                    [serviceUuid]: {
                        ...prevDevice.characteristicsByService[serviceUuid],
                        [charUuid]: { ...existingInService, ...resolvedFields },
                    },
                },
            };
        });
    }, []);

    // Service-aware sibling of updateCharValue: a fresh value also clears any stale lastWriteError
    // for that service's characteristic (issue #92, care point 1).
    const updateServiceCharacteristicValue = useCallback((serviceUuid: string, charUuid: string, newValue: string) => {
        updateServiceCharacteristicFields(serviceUuid, charUuid, { value: newValue, lastWriteError: null });
    }, [updateServiceCharacteristicFields]);

    // Service-aware sibling of setCharUpdateInProgress.
    const setServiceCharUpdateInProgress = useCallback((serviceUuid: string, charUuid: string, inProgress: boolean) => {
        updateServiceCharacteristicFields(serviceUuid, charUuid, { isUpdateInProgress: inProgress });
    }, [updateServiceCharacteristicFields]);

    // Service-aware sibling of writeToCharacteristic, for characteristics whose UUID is
    // intentionally reused across multiple services (e.g. UUID_IS_ACTIVE_CHARACTERISTIC, reused
    // identically across every animation service - see its doc comment in constants/bluetooth.ts).
    // Those characteristics are deliberately excluded from the flat characteristics map (a
    // duplicated UUID could only ever resolve to one, ambiguous, entry there), so
    // writeToCharacteristic/getCharacteristicInfo can't address them - this resolves the
    // CharacteristicInfo via characteristicsByService[serviceUuid][charUuid] instead.
    // writeToCharacteristic itself is untouched, so every other characteristic keeps its
    // existing 2-arg call pattern unchanged.
    const writeServiceCharacteristic = useCallback(async (
        serviceUuid: string,
        charUuid: string,
        newEncodedValue: string,
        options?: { skipOptimisticUpdate?: boolean; optimisticValue?: string }
    ): Promise<boolean> => {
        const charInfo = getServiceCharacteristicInfo(serviceUuid, charUuid);
        if (!charInfo) {
            console.log(`Characteristic ${charUuid} not found in service ${serviceUuid}`);
            return false;
        }

        // Clear any prior failure as the new attempt starts (issue #92, care point a).
        updateServiceCharacteristicFields(serviceUuid, charUuid, { isUpdateInProgress: true, lastWriteError: null });

        // Optimistic value update BEFORE the await, batched with isUpdateInProgress=true, so the
        // controlled "Is Active" Switch never flickers back to its old value mid-write (issue #91).
        // See the matching comment in writeToCharacteristic. Reverted on write rejection below.
        const previousValue = charInfo.value ?? '';
        // See writeToCharacteristic: `optimisticValue` lets a caller whose written bytes differ from
        // the device's canonical stored value (dropdown lists) still update instantly.
        const optimisticValue = options?.optimisticValue ?? newEncodedValue;
        if (!options?.skipOptimisticUpdate) {
            updateServiceCharacteristicValue(serviceUuid, charUuid, optimisticValue);
        }

        try {
            await charInfo.characteristic.writeWithResponse(newEncodedValue);
            return true;
        } catch (error) {
            // Compare-and-swap revert: only undo our optimistic value if nothing else (a device
            // notification, an overlapping write) changed it while the write was in flight. Runs
            // inside the state updater so it sees the latest value. See writeToCharacteristic.
            if (!options?.skipOptimisticUpdate) {
                updateServiceCharacteristicFields(serviceUuid, charUuid, current => current.value === optimisticValue ? { value: previousValue } : null);
            }
            console.log(`Error writing value to characteristic ${charUuid} in service ${serviceUuid}: ${error}`);
            updateServiceCharacteristicFields(serviceUuid, charUuid, { lastWriteError: describeWriteError(error) });
            return false;
        } finally {
            setServiceCharUpdateInProgress(serviceUuid, charUuid, false);
        }
    }, [getServiceCharacteristicInfo, setServiceCharUpdateInProgress, updateServiceCharacteristicValue, updateServiceCharacteristicFields]);

    const contextValue = useMemo(() => ({
        selectedDevice,
        setSelectedDevice,
        isScanning,
        setIsScanning,
        connectingDevice,
        setConnectingDevice,
        reconnectingDevice,
        setReconnectingDevice,
        reconnectGeneration,
        intentionalDisconnectRef,
        connectPromises,
        discoveryProgress,
        setDiscoveryProgress,
        writeToCharacteristic,
        getCharacteristicInfo,
        updateCharValue,
        getServiceCharacteristicInfo,
        updateServiceCharacteristicValue,
        writeServiceCharacteristic,
        monitorSubscriptions,
        disconnectSubscription,
        selectedDeviceRef,
    }), [
        selectedDevice, isScanning, connectingDevice, reconnectingDevice, discoveryProgress, writeToCharacteristic, getCharacteristicInfo, updateCharValue,
        getServiceCharacteristicInfo, updateServiceCharacteristicValue, writeServiceCharacteristic,
    ]);

    return (
        <BluetoothContext.Provider value={contextValue}>
            {children}
        </BluetoothContext.Provider>
    );
}

export function useBluetooth(): BluetoothContextType {
    const context = useContext(BluetoothContext);
    if (!context) throw new Error('useBluetooth must be used within BluetoothProvider');
    return context;
}