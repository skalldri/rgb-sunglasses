import {
    BLE_GATT_CPF_FORMAT_DROPDOWN_LIST,
    getCharacteristicName,
    getDescriptorName,
    getServiceName,
    UUID_ANIMATION_NAME_CHARACTERISTIC,
    UUID_CCC_DESCRIPTOR,
    UUID_CPF_DESCRIPTOR,
    UUID_CUD_DESCRIPTOR,
    UUID_IS_ACTIVE_CHARACTERISTIC,
    UUID_METADATA_CHARACTERISTIC,
} from "@/constants/bluetooth";
import { CharacteristicInfo, useBluetooth } from "@/context/bluetooth-context";
import { bleManager } from "@/hooks/ble-manager";
import { decodeUtf8FromBase64, MetadataBlobEntry, parseMetadataBlob } from "@/services/ble-value-codec";
import { SMP_CHARACTERISTIC_UUID, SMP_SERVICE_UUID } from "@/services/mcumgr";
import { useEffect, useRef, useState } from "react";
import { ConnectionPriority } from "react-native-ble-plx";

interface UseBleConnectionResult {
    isConnecting: boolean;
    // Resolves true only once the device is fully connected, discovered, and
    // selected; false if the attempt failed (the error is logged and the
    // half-open link cleaned up internally). Callers must gate navigation on the
    // result rather than assuming a resolved promise means "connected".
    connect: () => Promise<boolean>;
    disconnect: () => Promise<void>;
}

export function useBleConnection(macAddress: string, deviceName: string): UseBleConnectionResult {
    const {
        selectedDevice, setSelectedDevice, updateCharValue, updateServiceCharacteristicValue,
        monitorSubscriptions, disconnectSubscription, setDiscoveryProgress, setConnectingDevice,
    } = useBluetooth();

    const [isConnecting, setIsConnecting] = useState(false);

    // Keeps a live reference to selectedDevice so the disconnect listener always
    // sees the current value, not a stale closure snapshot.
    const selectedDeviceRef = useRef(selectedDevice);
    selectedDeviceRef.current = selectedDevice;

    // Guards against calling setIsConnecting after the component unmounts.
    const isMountedRef = useRef(true);
    useEffect(() => () => { isMountedRef.current = false; }, []);

    // Re-entrancy guard AND result-sharing, checked synchronously (unlike
    // isConnecting state, which is async - a second onPress delivered before the
    // first render commit sees isConnecting still false and disabled={isConnecting}
    // hasn't taken effect yet). A same-tick second connect() call reaching
    // bleManager.connectToDevice() for the same macAddress makes
    // react-native-ble-plx's native module dispose the FIRST call's pending
    // subscription (DisposableMap.replaceSubscription always disposes whatever was
    // already stored under that device's key) - rejecting the first promise with
    // BleErrorCode.OperationCancelled ("Operation was cancelled") while the second
    // call's establishConnection() is what actually completes on the real
    // BluetoothGatt. That produces exactly the split-brain symptom observed on
    // hardware: the firmware reports a live, fast-interval connection while the
    // app's connect() throws.
    //
    // Holding the in-flight PROMISE (not just a boolean) means a duplicate call
    // returns the real attempt's eventual result instead of resolving immediately:
    // the caller navigates on genuine success, and a double-tap doesn't push the
    // device-state screen off a no-op early return before anything is connected.
    const connectPromiseRef = useRef<Promise<boolean> | null>(null);

    function connect(): Promise<boolean> {
        // Synchronous dedup: the ref is assigned below before runConnect() yields
        // at its first await, so a second same-tick call sees it and shares the
        // same promise rather than starting a colliding connectToDevice().
        if (connectPromiseRef.current) {
            return connectPromiseRef.current;
        }
        const attempt = runConnect();
        connectPromiseRef.current = attempt;
        // Clear once settled so a later reconnect starts a fresh attempt. runConnect
        // resolves true/false and never rejects, so this never leaves a rejection
        // unhandled.
        attempt.finally(() => { connectPromiseRef.current = null; });
        return attempt;
    }

    async function runConnect(): Promise<boolean> {
        setIsConnecting(true);
        // Pin this device in the Connect screen's list for the whole attempt: it stops advertising
        // the moment its LE link comes up, so the scan-derived list would otherwise prune it
        // mid-pairing and unmount this row (with its progress indicator), making a still-in-progress
        // pair look failed (issue #158). Cleared in finally, once the attempt settles either way.
        setConnectingDevice({ mac: macAddress, name: deviceName });
        try {
            // A scan running concurrently with connectToDevice() can make the
            // connect operation itself get cancelled by the OS/library even
            // though the native link actually completes - leaving the app
            // thinking the connection failed while the board thinks it's
            // connected (and has stopped advertising, so no reconnect/rescan
            // can reach it either). stopDeviceScan is a safe no-op if nothing
            // is currently scanning (see CLAUDE.md's "Scan must stop before
            // connecting").
            bleManager.stopDeviceScan();

            // Connect in DISTINCT, SEQUENTIAL steps - establish the link, negotiate
            // MTU, then discover - rather than folding refreshGatt + requestMTU into
            // the single connectToDevice() promise chain (issue #90). Two things this
            // buys us, learned from a multi-day hardware investigation (native adb
            // logcat BLE traces + a firmware `bt_state` shell command that prints the
            // negotiated ATT MTU):
            //
            // 1. requestMTU is its own awaited step (not inline in connectToDevice), so
            //    a slow/failed MTU exchange can't blow the whole connect timeout, and a
            //    failure here is non-fatal (reads/writes fragment at any MTU; only large
            //    notify payloads need the 247 bump - e.g. the dropdown-list
            //    characteristics, which silently fail firmware-side at the 23-byte
            //    default because bt_gatt_notify() can't fragment across ATT PDUs).
            //
            // 2. NO refreshGatt. It calls Android's BluetoothGatt.refresh(), which wipes
            //    the on-device GATT cache and forces a full re-discovery on EVERY
            //    connect - pure overhead when the cache is valid (the normal case: the
            //    firmware GATT layout is stable between reflashes). It was originally
            //    added to survive a firmware GATT-layout change on a bonded phone, but
            //    hardware testing proved it does NOT actually help that case: on this
            //    OnePlus/OxygenOS stack a bonded device with a STALE cache hangs
            //    discovery no matter what (refreshGatt on or off, MTU before or after) -
            //    `bt_state` shows the board CONNECTED + encrypted (L4) but stuck at
            //    ATT MTU 23, and the app's discovery times out. Android does NOT honor
            //    the firmware's Service Changed / DB-hash to recover (verified: added a
            //    characteristic, reflashed without re-pairing -> hang). The ONLY reliable
            //    recovery from a stale bonded cache is forget+re-pair on the phone (see
            //    the Known-Issues entry in app/CLAUDE.md). So refreshGatt bought nothing
            //    for the stale case and taxed every healthy connect - dropped.
            //
            // retry: the first connectToDevice() to a just-rebooted bonded board can
            // still fail at the controller level (HCI 0x3E, reason=62 in dumpsys) with
            // the OS retrying underneath; a second attempt attaches cleanly. Each
            // failed attempt's half-open BluetoothGatt is force-closed before retrying
            // so the next connectGatt doesn't queue behind a zombie client.
            let deviceConnection = null;
            const kConnectAttempts = 2;
            for (let attempt = 1; attempt <= kConnectAttempts; attempt++) {
                try {
                    // Barebones: link only. No refreshGatt, no inline requestMTU (both
                    // reasons above); MTU is negotiated as its own step below.
                    // 60s (not 15s): a first-time pair has to wait for the user (or the
                    // /re-pair autoresponder) to accept Android's pairing dialog before
                    // the encrypted link comes up and this resolves — 15s raced that.
                    deviceConnection = await bleManager.connectToDevice(macAddress, { timeout: 60000 });
                    break;
                } catch (error) {
                    console.log(`connectToDevice attempt ${attempt}/${kConnectAttempts} failed for ${macAddress}:`, error);
                    if (attempt === kConnectAttempts) {
                        throw error;
                    }
                    // Close the failed attempt's half-open native GATT client before
                    // retrying - a timed-out connectToDevice() does not reliably close
                    // the BluetoothGatt it opened, and a still-registered zombie client
                    // blocks the next connectGatt for the same device.
                    try {
                        await bleManager.cancelDeviceConnection(macAddress);
                    } catch {
                        // Expected when ble-plx never got far enough to consider the
                        // device connected - nothing to cancel is fine.
                    }
                    await new Promise(resolve => setTimeout(resolve, 1000));
                }
            }
            if (!deviceConnection) {
                // Unreachable (the loop either breaks with a connection or throws), but
                // keeps TypeScript's null-narrowing happy for everything below.
                throw new Error(`connectToDevice(${macAddress}) returned no connection`);
            }

            // MTU exchange as its own step (see the sequencing block above). Non-fatal:
            // reads/writes fragment at any MTU, only large notify payloads need the bump.
            try {
                await deviceConnection.requestMTU(247);
            } catch (error) {
                console.log(`Could not negotiate MTU for ${macAddress}:`, error);
            }

            // Discovery below does ~170+ sequential GATT reads (one per characteristic/descriptor -
            // can't be parallelized, Android only allows one outstanding GATT op per connection).
            // Without this, the connection runs at the OS default interval (~30-50ms), making every
            // one of those reads slow. High priority requests ~7.5-15ms instead. Android-only effect
            // (see fw/src/bluetooth.cpp's matching bt_conn_le_param_update for the firmware-side
            // request); non-fatal if it fails, discovery just runs at whatever interval is already
            // negotiated.
            try {
                await deviceConnection.requestConnectionPriority(ConnectionPriority.High);
            } catch (error) {
                console.log(`Could not request high connection priority for ${macAddress}:`, error);
            }

            await deviceConnection.discoverAllServicesAndCharacteristics();
            const services = await deviceConnection.services();

            const characteristicsByService: Record<string, Record<string, CharacteristicInfo>> = {};
            const characteristics: Record<string, CharacteristicInfo> = {};
            const serviceCharacteristics: Record<string, string[]> = {};
            const serviceDisplayNames: Record<string, string> = {};

            if (services) {
                // Pre-pass: characteristicsForService() just returns metadata already gathered by
                // discoverAllServicesAndCharacteristics() above (no extra ATT round-trips), so doing
                // it once upfront lets us show real "N of M characteristics" progress for the actual
                // per-characteristic read loop below, instead of only an indeterminate spinner.
                const serviceCharsList = await Promise.all(
                    services.map(service => deviceConnection.characteristicsForService(service.uuid))
                );
                // Excludes the bulk metadata characteristic (issue #41 follow-up) from the count:
                // it's never individually processed in the loop below (see displayChars), so
                // including it here would make `total` permanently 1 higher than `current` can
                // ever reach for any service that has one.
                const totalCharacteristics = serviceCharsList.reduce(
                    (sum, chars) => sum + chars.filter(c => c.uuid !== UUID_METADATA_CHARACTERISTIC).length,
                    0
                );
                let processedCharacteristics = 0;
                setDiscoveryProgress({ current: 0, total: totalCharacteristics });

                for (let i = 0; i < services.length; i++) {
                    const service = services[i];
                    const serviceChars = serviceCharsList[i];
                    const characteristicInfos: Record<string, CharacteristicInfo> = {};
                    const charUuids: string[] = [];

                    // The bulk metadata characteristic (issue #41 follow-up) is an app-only
                    // discovery optimization - never shown in the UI, same treatment as
                    // UUID_ANIMATION_NAME_CHARACTERISTIC below, just excluded entirely rather
                    // than redirected into serviceDisplayNames.
                    const metadataCharacteristic = serviceChars.find(c => c.uuid === UUID_METADATA_CHARACTERISTIC);
                    const displayChars = serviceChars.filter(c => c.uuid !== UUID_METADATA_CHARACTERISTIC);

                    // Bulk-read fast path: one ATT read for this service's CUD names + CPF
                    // formats instead of two descriptor reads per characteristic. Falls back to
                    // the unchanged per-descriptor path below on any read failure, version
                    // mismatch, or entry-count mismatch (see parseMetadataBlob() for what counts
                    // as malformed) - services without this characteristic (e.g. the third-party
                    // McuMgr service, or any board built with CONFIG_APP_BT_METADATA_CHARACTERISTIC
                    // disabled) take the fallback path automatically, with zero special-casing.
                    //
                    // ORDERING ASSUMPTION: this zips bulkMetadata[j] to displayChars[j]
                    // positionally, which assumes characteristicsForService() returns
                    // characteristics in firmware GATT declaration order. This holds because ATT
                    // "Read By Type" (used internally by characteristic discovery) is spec-required
                    // to return attributes in ascending handle order, and handles are assigned in
                    // exactly the order BtGattServer's Providers... pack is declared - see
                    // MetadataBlobBuilder's doc comment in fw/src/bluetooth/bt_service_cpp.h for the
                    // full rationale. The one UNVERIFIED link in this chain is react-native-ble-plx's
                    // Android module: it must pass the native BluetoothGatt discovery result through
                    // to characteristicsForService() without any client-side re-sort (e.g. by UUID
                    // string) - a library version bump, not an ATT spec violation, is the one way
                    // this guarantee could silently break. Note the entry-count check below catches
                    // a COUNT mismatch but NOT a same-count reordering - that residual risk is
                    // accepted (see the plan for issue #41's metadata-characteristic follow-up).
                    let bulkMetadata: MetadataBlobEntry[] | null = null;
                    if (metadataCharacteristic) {
                        try {
                            const read = await metadataCharacteristic.read();
                            bulkMetadata = parseMetadataBlob(read.value);
                        } catch (error) {
                            console.log(`Could not read bulk metadata characteristic for ${getServiceName(service.uuid)}:`, error);
                        }

                        if (bulkMetadata && bulkMetadata.length !== displayChars.length) {
                            console.log(
                                `Bulk metadata count mismatch for ${getServiceName(service.uuid)}: got ${bulkMetadata.length}, expected ${displayChars.length}. Falling back to per-descriptor reads.`
                            );
                            bulkMetadata = null;
                        }
                    }

                    for (let j = 0; j < displayChars.length; j++) {
                        const characteristic = displayChars[j];
                        const charInfo: CharacteristicInfo = {
                            characteristic,
                            value: null,
                            name: null,
                            cpfFormat: null,
                            isUpdateInProgress: false,
                        };

                        if (bulkMetadata) {
                            charInfo.name = bulkMetadata[j].name;
                            charInfo.cpfFormat = bulkMetadata[j].cpfFormat;
                            console.log(`Characteristic: ${getCharacteristicName(characteristic.uuid)} (from bulk metadata): name="${charInfo.name}", cpfFormat=${charInfo.cpfFormat}`);
                        } else {
                            const descriptors = await service.descriptorsForCharacteristic(characteristic.uuid);
                            console.log(`Characteristic: ${getCharacteristicName(characteristic.uuid)}, Descriptors: ${descriptors.length}`);

                            for (const descriptor of descriptors) {
                                // CCC just reflects local notification-subscription state (always 0x0000
                                // here, before connect() ever subscribes via characteristic.monitor()
                                // below) - the read result was never stored or used, only logged. Skip
                                // it to save one ATT round-trip per notifiable characteristic.
                                if (descriptor.uuid === UUID_CCC_DESCRIPTOR) {
                                    continue;
                                }

                                console.log(`Descriptor UUID: ${getDescriptorName(descriptor.uuid)}`);

                                let readDescriptor;
                                try {
                                    readDescriptor = await descriptor.read();
                                } catch (error) {
                                    console.log(`Could not read descriptor ${getDescriptorName(descriptor.uuid)}:`, error);
                                    continue;
                                }
                                console.log(`Descriptor Value: ${readDescriptor.value}`);

                                if (descriptor.uuid === UUID_CUD_DESCRIPTOR) {
                                    charInfo.name = atob(readDescriptor.value || '');
                                    console.log(`CUD Descriptor Value (decoded): ${charInfo.name}`);
                                }

                                if (descriptor.uuid === UUID_CPF_DESCRIPTOR) {
                                    const decoded = atob(readDescriptor.value || '');
                                    charInfo.cpfFormat = decoded.charCodeAt(0);
                                    const hex = Array.from(decoded, char => char.charCodeAt(0).toString(16).padStart(2, '0')).join(' ');
                                    console.log(`CPF Descriptor Value (hex): ${hex}`);
                                }
                            }
                        }

                        try {
                            const readCharacteristic = await characteristic.read();
                            charInfo.value = readCharacteristic.value;
                            console.log(`Characteristic Value: ${charInfo.value}`);
                        } catch (error) {
                            console.log(`Could not read characteristic ${getCharacteristicName(characteristic.uuid)}:`, error);
                        }

                        characteristicInfos[characteristic.uuid] = charInfo;

                        if (characteristic.uuid === UUID_ANIMATION_NAME_CHARACTERISTIC) {
                            // This UUID is intentionally reused across every animation service (see
                            // constants/bluetooth.ts), so it must not go into the flat characteristics
                            // map or serviceCharacteristics: both are keyed/searched by UUID alone and
                            // would collide across services, making characteristic-to-service lookups
                            // ambiguous. Its value is only ever needed here, to derive the display name.
                            if (charInfo.value) {
                                try {
                                    serviceDisplayNames[service.uuid] = decodeUtf8FromBase64(charInfo.value).replace(/\0+$/, '');
                                } catch (error) {
                                    console.log(`Could not decode animation name for service ${service.uuid}:`, error);
                                }
                            }
                        } else if (characteristic.uuid === UUID_IS_ACTIVE_CHARACTERISTIC) {
                            // Also reused identically across every animation service (see
                            // constants/bluetooth.ts) - same collision risk as Animation Name, so it's
                            // excluded from the flat maps too. Unlike Animation Name it stays
                            // read/write/notifiable per-service, addressed via
                            // characteristicsByService[serviceUuid][charUuid] and the service-aware
                            // getServiceCharacteristicInfo/writeServiceCharacteristic context helpers.
                        } else {
                            characteristics[characteristic.uuid] = charInfo;
                            charUuids.push(characteristic.uuid);
                        }

                        processedCharacteristics++;
                        setDiscoveryProgress({ current: processedCharacteristics, total: totalCharacteristics });
                    }

                    characteristicsByService[service.uuid] = characteristicInfos;
                    serviceCharacteristics[service.uuid] = charUuids;
                    console.log(`Service UUID: ${getServiceName(service.uuid)}, Characteristics: ${Object.keys(characteristicInfos).length}`);
                }
            }

            setSelectedDevice({
                name: deviceName,
                mac: macAddress,
                device: deviceConnection,
                services,
                characteristicsByService,
                characteristics,
                serviceCharacteristics,
                serviceDisplayNames,
            });

            // Set up monitors for all notifiable characteristics except SMP
            // (SMP has its own monitoring managed by McuMgrClient)
            console.log('Setting up characteristic monitors...');
            Object.entries(characteristicsByService).forEach(([serviceUuid, chars]) => {
                const serviceName = getServiceName(serviceUuid);
                Object.entries(chars).forEach(([charUuid, charInfo]) => {
                    const charName = charInfo.name || getCharacteristicName(charUuid);
                    if (
                        charInfo.characteristic.isNotifiable &&
                        !(serviceUuid === SMP_SERVICE_UUID && charUuid === SMP_CHARACTERISTIC_UUID)
                    ) {
                        console.log(`Setting up monitor for notifiable characteristic: ${serviceName} > ${charName}`);

                        const subscription = charInfo.characteristic.monitor((error, characteristic) => {
                            console.log(`Monitor called for ${charName}`);

                            if (error) {
                                const errorStr = error?.message || String(error);
                                if (
                                    errorStr.includes('cancelled') || errorStr.includes('Cancelled') ||
                                    errorStr.includes('Disconnect') || errorStr.includes('disconnect')
                                ) {
                                    console.log(`Monitor for ${charName}: ${errorStr.includes('cancel') ? 'cancelled' : 'disconnected'}`);
                                    return;
                                }
                                console.error(`Notification error for ${charName}:`, error);
                                return;
                            }

                            if (characteristic && characteristic.value) {
                                console.log(`📡 Notification received for ${charName}: ${characteristic.value}`);

                                if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_DROPDOWN_LIST) {
                                    // Dropdown-list characteristics only notify their first
                                    // option (the new selection), not the full canonical
                                    // "selected\nothers..." list - bt_gatt_notify can't fragment
                                    // a value across multiple ATT PDUs the way a read can, so
                                    // notifying the whole list would scale notify payload size
                                    // (and failure risk) with the total option count instead of
                                    // with what actually changed. Re-read to get the full,
                                    // correctly-ordered value instead of trusting the notified
                                    // bytes directly. See fw/CLAUDE.md (BtGattNotifyTraits).
                                    characteristic.read()
                                        .then(read => {
                                            if (read.value) updateCharValue(charUuid, read.value);
                                        })
                                        .catch(err => console.log(`Failed to re-read ${charName} after notification:`, err));
                                } else if (charUuid === UUID_IS_ACTIVE_CHARACTERISTIC) {
                                    // Reused identically across every animation service, so it's
                                    // excluded from the flat characteristics map (see the
                                    // discovery loop above) - update it via the service-aware
                                    // path, keyed by this specific service, instead.
                                    updateServiceCharacteristicValue(serviceUuid, charUuid, characteristic.value);
                                } else {
                                    updateCharValue(charUuid, characteristic.value);
                                }
                            }
                        });

                        monitorSubscriptions.current.push(subscription);
                    }
                });
            });
            console.log(`Set up ${monitorSubscriptions.current.length} characteristic monitors`);

            // Register the disconnect listener using selectedDeviceRef so the callback
            // always reads the current mcuMgrClient, not a stale closure snapshot.
            disconnectSubscription.current = bleManager.onDeviceDisconnected(macAddress, (error, device) => {
                if (error) {
                    console.log(`Device disconnection error for ${macAddress}:`, error);
                }

                if (device && device.id === macAddress) {
                    console.log(`Device disconnected: ${deviceName} (${macAddress})`);

                    console.log(`Cleaning up ${monitorSubscriptions.current.length} characteristic monitors on disconnect`);
                    monitorSubscriptions.current.forEach(sub => sub.remove());
                    monitorSubscriptions.current = [];

                    if (selectedDeviceRef.current?.mcuMgrClient) {
                        try {
                            selectedDeviceRef.current.mcuMgrClient.destroy();
                        } catch (e) {
                            console.log('Error destroying MCUmgr client:', e);
                        }
                    }

                    setSelectedDevice(null);
                    disconnectSubscription.current = null;
                }
            });

            console.log('Pairing complete');
            return true;
        } catch (error) {
            console.error(`Connection failed for ${macAddress}:`, error);
            // Discovery can fail partway through, after the native BLE link is already
            // established. Without an explicit disconnect here, the device is left connected
            // at the OS level (so it stops advertising) while the app still thinks it's
            // unconnected, making it impossible to scan for or reconnect to.
            try {
                await bleManager.cancelDeviceConnection(macAddress);
            } catch (disconnectError) {
                console.log(`Error cancelling connection for ${macAddress}:`, disconnectError);
            }
            return false;
        } finally {
            setDiscoveryProgress(null);
            // Release the list pin - but only if it still points at THIS device (compare-and-swap).
            // connectingDevice is a single shared context slot and each device row runs its own
            // connect(), so if a connect to another board started while this one was in flight and
            // overwrote the slot, this attempt settling must NOT null out that other in-flight pin
            // (which would un-pin it mid-pairing and reintroduce issue #158 for it). On success the
            // caller navigates to the Controls screen (the device is now in selectedDevice); on
            // failure the device returns to normal scan-driven pruning. Either way this attempt's
            // hold is done here.
            setConnectingDevice(prev => (prev?.mac === macAddress ? null : prev));
            if (isMountedRef.current) setIsConnecting(false);
        }
    }

    async function disconnect(): Promise<void> {
        setIsConnecting(true);
        try {
            console.log(`Disconnecting from device: ${deviceName} (${macAddress})`);

            if (disconnectSubscription.current) {
                disconnectSubscription.current.remove();
                disconnectSubscription.current = null;
            }

            console.log(`Cleaning up ${monitorSubscriptions.current.length} characteristic monitors`);
            monitorSubscriptions.current.forEach(sub => sub.remove());
            monitorSubscriptions.current = [];

            await bleManager.cancelDeviceConnection(macAddress);
            setSelectedDevice(null);
        } finally {
            if (isMountedRef.current) setIsConnecting(false);
        }
    }

    return { isConnecting, connect, disconnect };
}
