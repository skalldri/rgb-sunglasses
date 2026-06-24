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
} from "@/constants/bluetooth";
import { CharacteristicInfo, useBluetooth } from "@/context/bluetooth-context";
import { bleManager } from "@/hooks/ble-manager";
import { decodeUtf8FromBase64 } from "@/services/ble-value-codec";
import { SMP_CHARACTERISTIC_UUID, SMP_SERVICE_UUID } from "@/services/mcumgr";
import { useEffect, useRef, useState } from "react";
import { ConnectionPriority } from "react-native-ble-plx";

interface UseBleConnectionResult {
    isConnecting: boolean;
    connect: () => Promise<void>;
    disconnect: () => Promise<void>;
}

export function useBleConnection(macAddress: string, deviceName: string): UseBleConnectionResult {
    const {
        selectedDevice, setSelectedDevice, updateCharValue, updateServiceCharacteristicValue,
        monitorSubscriptions, disconnectSubscription, setDiscoveryProgress,
    } = useBluetooth();

    const [isConnecting, setIsConnecting] = useState(false);

    // Keeps a live reference to selectedDevice so the disconnect listener always
    // sees the current value, not a stale closure snapshot.
    const selectedDeviceRef = useRef(selectedDevice);
    selectedDeviceRef.current = selectedDevice;

    // Guards against calling setIsConnecting after the component unmounts.
    const isMountedRef = useRef(true);
    useEffect(() => () => { isMountedRef.current = false; }, []);

    async function connect(): Promise<void> {
        setIsConnecting(true);
        try {
            // Android persists a handle-based GATT attribute cache per bonded device. Any firmware
            // change that adds/removes services or characteristics shifts attribute handles for
            // everything declared after the change, so a stale cache makes Android read descriptors
            // by the wrong handle (GATT_INVALID_HANDLE) until it's refreshed.
            //
            // requestMTU: without this, the connection stays at the default ATT_MTU (23 bytes, ~20
            // bytes usable). bt_gatt_notify() can't fragment a value across multiple PDUs the way
            // long writes/reads can, so any notified value over ~20 bytes (e.g. the dropdown-list
            // characteristics) silently fails firmware-side ("No ATT channel for MTU ...", a warning
            // only - no error surfaces to the app). 247 matches the common BLE 4.2+ default data
            // length and comfortably covers every current notifiable characteristic's payload.
            const deviceConnection = await bleManager.connectToDevice(macAddress, {
                refreshGatt: "OnConnected",
                requestMTU: 247,
            });

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
                const totalCharacteristics = serviceCharsList.reduce((sum, chars) => sum + chars.length, 0);
                let processedCharacteristics = 0;
                setDiscoveryProgress({ current: 0, total: totalCharacteristics });

                for (let i = 0; i < services.length; i++) {
                    const service = services[i];
                    const serviceChars = serviceCharsList[i];
                    const characteristicInfos: Record<string, CharacteristicInfo> = {};
                    const charUuids: string[] = [];

                    console.log(`START processing Service UUID: ${getServiceName(service.uuid)}`);

                    for (const characteristic of serviceChars) {
                        const descriptors = await service.descriptorsForCharacteristic(characteristic.uuid);
                        console.log(`Characteristic: ${getCharacteristicName(characteristic.uuid)}, Descriptors: ${descriptors.length}`);

                        const charInfo: CharacteristicInfo = {
                            characteristic,
                            value: null,
                            name: null,
                            cpfFormat: null,
                            isUpdateInProgress: false,
                        };

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
        } finally {
            setDiscoveryProgress(null);
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
