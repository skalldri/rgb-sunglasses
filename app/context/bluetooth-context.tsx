import { describeWriteError } from "@/services/ble-errors";
import { McuMgrClient } from "@/services/mcumgr";
import { createContext, ReactNode, useCallback, useContext, useMemo, useRef, useState } from "react";
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

type BluetoothContextType = {
    selectedDevice: BluetoothContextDevice | null;
    setSelectedDevice: (device: BluetoothContextDevice | null) => void;
    isScanning: boolean;
    setIsScanning: (scanning: boolean) => void;
    // Characteristic-query progress during connect()'s discovery walk; null when not connecting
    // or once discovery has finished. See use-ble-connection.ts.
    discoveryProgress: DiscoveryProgress | null;
    setDiscoveryProgress: (progress: DiscoveryProgress | null) => void;
    writeToCharacteristic: (
        charUuid: string,
        newEncodedValue: string,
        options?: { skipOptimisticUpdate?: boolean }
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
        options?: { skipOptimisticUpdate?: boolean }
    ) => Promise<boolean>;
    // Monitor subscription management (persists across navigation)
    monitorSubscriptions: React.MutableRefObject<Subscription[]>;
    disconnectSubscription: React.MutableRefObject<Subscription | null>;
};

const BluetoothContext = createContext<BluetoothContextType | undefined>(undefined);


export function BluetoothProvider({ children }: { children: ReactNode }) {
    const [selectedDevice, setSelectedDevice] = useState<BluetoothContextDevice | null>(null);
    const [isScanning, setIsScanning] = useState<boolean>(false);
    const [discoveryProgress, setDiscoveryProgress] = useState<DiscoveryProgress | null>(null);

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
    const writeToCharacteristic = useCallback(async (
        charUuid: string,
        newEncodedValue: string,
        options?: { skipOptimisticUpdate?: boolean }
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
        if (!options?.skipOptimisticUpdate) {
            updateCharValue(charUuid, newEncodedValue);
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
                updateCharFields(charUuid, current => current.value === newEncodedValue ? { value: previousValue } : null);
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
        options?: { skipOptimisticUpdate?: boolean }
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
        if (!options?.skipOptimisticUpdate) {
            updateServiceCharacteristicValue(serviceUuid, charUuid, newEncodedValue);
        }

        try {
            await charInfo.characteristic.writeWithResponse(newEncodedValue);
            return true;
        } catch (error) {
            // Compare-and-swap revert: only undo our optimistic value if nothing else (a device
            // notification, an overlapping write) changed it while the write was in flight. Runs
            // inside the state updater so it sees the latest value. See writeToCharacteristic.
            if (!options?.skipOptimisticUpdate) {
                updateServiceCharacteristicFields(serviceUuid, charUuid, current => current.value === newEncodedValue ? { value: previousValue } : null);
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
    }), [
        selectedDevice, isScanning, discoveryProgress, writeToCharacteristic, getCharacteristicInfo, updateCharValue,
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