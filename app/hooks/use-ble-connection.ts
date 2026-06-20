import {
    getCharacteristicName,
    getDescriptorName,
    getServiceName,
    UUID_ANIMATION_NAME_CHARACTERISTIC,
    UUID_CCC_DESCRIPTOR,
    UUID_CPF_DESCRIPTOR,
    UUID_CUD_DESCRIPTOR,
} from "@/constants/bluetooth";
import { CharacteristicInfo, useBluetooth } from "@/context/bluetooth-context";
import { bleManager } from "@/hooks/ble-manager";
import { decodeUtf8FromBase64 } from "@/services/ble-value-codec";
import { SMP_CHARACTERISTIC_UUID, SMP_SERVICE_UUID } from "@/services/mcumgr";
import { useEffect, useRef, useState } from "react";

interface UseBleConnectionResult {
    isConnecting: boolean;
    connect: () => Promise<void>;
    disconnect: () => Promise<void>;
}

export function useBleConnection(macAddress: string, deviceName: string): UseBleConnectionResult {
    const { selectedDevice, setSelectedDevice, updateCharValue, monitorSubscriptions, disconnectSubscription } =
        useBluetooth();

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
            const deviceConnection = await bleManager.connectToDevice(macAddress, { refreshGatt: "OnConnected" });
            await deviceConnection.discoverAllServicesAndCharacteristics();
            const services = await deviceConnection.services();

            const characteristicsByService: Record<string, Record<string, CharacteristicInfo>> = {};
            const characteristics: Record<string, CharacteristicInfo> = {};
            const serviceCharacteristics: Record<string, string[]> = {};
            const serviceDisplayNames: Record<string, string> = {};

            if (services) {
                for (const service of services) {
                    const serviceChars = await deviceConnection.characteristicsForService(service.uuid);
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

                            if (descriptor.uuid === UUID_CCC_DESCRIPTOR) {
                                const decoded = atob(readDescriptor.value || '');
                                const hex = Array.from(decoded, char => char.charCodeAt(0).toString(16).padStart(2, '0')).join(' ');
                                console.log(`CCC Descriptor Value (hex): ${hex}`);
                            }
                        }

                        try {
                            const readCharacteristic = await characteristic.read();
                            charInfo.value = readCharacteristic.value;
                            console.log(`Characteristic Value: ${charInfo.value}`);
                        } catch (error) {
                            console.log(`Could not read characteristic ${getCharacteristicName(characteristic.uuid)}:`, error);
                        }

                        if (characteristic.uuid === UUID_ANIMATION_NAME_CHARACTERISTIC && charInfo.value) {
                            serviceDisplayNames[service.uuid] = decodeUtf8FromBase64(charInfo.value);
                        }

                        characteristicInfos[characteristic.uuid] = charInfo;
                        characteristics[characteristic.uuid] = charInfo;
                        charUuids.push(characteristic.uuid);
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
                                updateCharValue(charUuid, characteristic.value);
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
