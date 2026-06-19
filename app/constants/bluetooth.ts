
export const KnownServiceIds: { [key: string]: string } = {
    "12345678-1234-5678-0001-56789abc0000": "Core Config Service",
    "12345678-1234-5678-0100-56789abd0000": "ZigZag Animation Service",
    "12345678-1234-5678-0200-56789abd0000": "Text Animation Service",
    "12345678-1234-5678-0500-56789abd0000": "Rainbow Animation Service",
    "12345678-1234-5678-0700-56789abd0000": "MyEyes Animation Service",
    "12345678-1234-5678-0800-56789abd0000": "Beat Animation Service",
    "12345678-1234-5678-0900-56789abd0000": "FFT Bars Animation Service",
    "12345678-1234-5678-1234-56789abcdef0": "Badge Name Service",
    "8d53dc1d-1db7-4cd3-868b-8a527460aa84": "McuMgr Service",
    "57a70000-9350-11ed-a1eb-0242ac120002": "Nordic Status Message Service",
    "00001801-0000-1000-8000-00805f9b34fb": "Generic Attribute Service",
    "00001800-0000-1000-8000-00805f9b34fb": "Generic Access Service",
    "deadbeef-1234-5678-1234-56789abcdef0": "Text Animation Now Playing Service",
};

// Fixed characteristic UUID, identical across every animation service, exposing that
// animation's human-readable name. Must match kAnimationNameCharacteristicUuid in
// fw/src/bluetooth/bt_service_cpp.h.
export const UUID_ANIMATION_NAME_CHARACTERISTIC = "12345678-1234-5678-aaaa-56789abd0000";

export const KnownCharacteristicIds: { [key: string]: string } = {
    "00002a05-0000-1000-8000-00805f9b34fb": "Service Changed Characteristic",
    "00002a00-0000-1000-8000-00805f9b34fb": "Device Name Characteristic",
    "00002a01-0000-1000-8000-00805f9b34fb": "Appearance Characteristic",
    "00002a04-0000-1000-8000-00805f9b34fb": "Peripheral Preferred Connection Parameters Characteristic",
    "da2e7828-fbce-4e01-ae9e-261174997c48": "McuMgr Control Characteristic",
};

export const KnownDescriptorIds: { [key: string]: string } = {
    "00002904-0000-1000-8000-00805f9b34fb": "Characteristic Presentation Format Descriptor",
    "00002901-0000-1000-8000-00805f9b34fb": "Characteristic User Description Descriptor",
    "00002902-0000-1000-8000-00805f9b34fb": "Client Characteristic Configuration Descriptor",
};

export const BLE_GATT_CPF_FORMAT_RFU = 0x00;
export const BLE_GATT_CPF_FORMAT_BOOLEAN = 0x01;
export const BLE_GATT_CPF_FORMAT_2BIT = 0x02;
export const BLE_GATT_CPF_FORMAT_NIBBLE = 0x03;
export const BLE_GATT_CPF_FORMAT_UINT8 = 0x04;
export const BLE_GATT_CPF_FORMAT_UINT12 = 0x05;
export const BLE_GATT_CPF_FORMAT_UINT16 = 0x06;
export const BLE_GATT_CPF_FORMAT_UINT24 = 0x07;
export const BLE_GATT_CPF_FORMAT_UINT32 = 0x08;
export const BLE_GATT_CPF_FORMAT_UINT48 = 0x09;
export const BLE_GATT_CPF_FORMAT_UINT64 = 0x0A;
export const BLE_GATT_CPF_FORMAT_UINT128 = 0x0B;
export const BLE_GATT_CPF_FORMAT_SINT8 = 0x0C;
export const BLE_GATT_CPF_FORMAT_SINT12 = 0x0D;
export const BLE_GATT_CPF_FORMAT_SINT16 = 0x0E;
export const BLE_GATT_CPF_FORMAT_SINT24 = 0x0F;
export const BLE_GATT_CPF_FORMAT_SINT32 = 0x10;
export const BLE_GATT_CPF_FORMAT_SINT48 = 0x11;
export const BLE_GATT_CPF_FORMAT_SINT64 = 0x12;
export const BLE_GATT_CPF_FORMAT_SINT128 = 0x13;
export const BLE_GATT_CPF_FORMAT_FLOAT32 = 0x14;
export const BLE_GATT_CPF_FORMAT_FLOAT64 = 0x15;
export const BLE_GATT_CPF_FORMAT_SFLOAT = 0x16;
export const BLE_GATT_CPF_FORMAT_FLOAT = 0x17;
export const BLE_GATT_CPF_FORMAT_DUINT16 = 0x18;
export const BLE_GATT_CPF_FORMAT_UTF8S = 0x19;
export const BLE_GATT_CPF_FORMAT_UTF16S = 0x1A;
export const BLE_GATT_CPF_FORMAT_STRUCT = 0x1B;

export const BLE_GATT_CPF_FORMAT_CUSTOM_COLOR = 0xE0; // Custom format for RGB color value (not standard)

export function getServiceName(serviceId: string): string {
    return KnownServiceIds[serviceId] || serviceId;
}

export function getCharacteristicName(characteristicId: string): string {
    return KnownCharacteristicIds[characteristicId] || characteristicId;
}

export function getDescriptorName(descriptorId: string): string {
    return KnownDescriptorIds[descriptorId] || descriptorId;
}

// Characteristic Presentation Format Descriptor UUID
export const UUID_CPF_DESCRIPTOR = "00002904-0000-1000-8000-00805f9b34fb";
// Characteristic User Description Descriptor UUID
export const UUID_CUD_DESCRIPTOR = "00002901-0000-1000-8000-00805f9b34fb";
// Client Characteristic Configuration Descriptor UUID
export const UUID_CCC_DESCRIPTOR = "00002902-0000-1000-8000-00805f9b34fb";