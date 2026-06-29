
// Services with no firmware-introspectable name (no "Animation Name" characteristic), so their
// display name has to come from a static map. Animation services are deliberately NOT listed
// here: they're named live via UUID_ANIMATION_NAME_CHARACTERISTIC (see use-ble-connection.ts),
// which is the whole point of issue #39 — a hardcoded entry here would go stale the moment
// firmware adds/renames an animation, exactly like this map silently did for Bad Apple/Nyan Cat
// before that fix landed.
export const KnownServiceIds: { [key: string]: string } = {
    "12345678-1234-5678-0001-56789abc0000": "Core Config",
    "12345678-1234-5678-0002-56789abc0000": "Audio Analysis Config",
    "12345678-1234-5678-0003-56789abc0000": "Bootloader Info",
    "12345678-1234-5678-0004-56789abc0000": "MCUboot Updater",
    "8d53dc1d-1db7-4cd3-868b-8a527460aa84": "McuMgr Service",
    "57a70000-9350-11ed-a1eb-0242ac120002": "Nordic Status Message Service",
    "00001801-0000-1000-8000-00805f9b34fb": "Generic Attribute Service",
    "00001800-0000-1000-8000-00805f9b34fb": "Generic Access Service",
};

// Well-known non-animation service UUIDs that receive special treatment in device-state/index.tsx.
export const UUID_MCUBOOT_INFO_SERVICE = "12345678-1234-5678-0003-56789abc0000";

// MCUboot BLE updater service (service ID 4). Proto0 only.
// Must match kServiceUuid / kStatusUuid / kDataUuid / kControlUuid in
// fw/src/bluetooth/mcuboot_updater_service.cpp.
export const UUID_MCUBOOT_UPDATER_SERVICE = "12345678-1234-5678-0004-56789abc0000";
export const UUID_MCUBOOT_UPDATER_STATUS  = "12345678-1234-5678-0004-56789abc0001";
export const UUID_MCUBOOT_UPDATER_DATA    = "12345678-1234-5678-0004-56789abc0002";
export const UUID_MCUBOOT_UPDATER_CONTROL = "12345678-1234-5678-0004-56789abc0003";

// Fixed characteristic UUID, identical across every animation service, exposing that
// animation's human-readable name. Must match kAnimationNameCharacteristicUuid in
// fw/src/bluetooth/bt_service_cpp.h.
export const UUID_ANIMATION_NAME_CHARACTERISTIC = "12345678-1234-5678-aaaa-56789abd0000";

// Fixed characteristic UUID, identical across every animation service, exposing that
// animation's boolean "is active" state. Must match kIsActiveCharacteristicUuid in
// fw/src/bluetooth/bt_service_cpp.h. Like UUID_ANIMATION_NAME_CHARACTERISTIC, this UUID is
// intentionally reused across every animation service, so it must never go into the flat
// characteristics/serviceCharacteristics maps (keyed by bare UUID, which would collide) — only
// into characteristicsByService, and written/read via the service-aware
// writeServiceCharacteristic/getServiceCharacteristicInfo context helpers.
export const UUID_IS_ACTIVE_CHARACTERISTIC = "12345678-1234-5678-bbbb-56789abd0000";

// Fixed characteristic UUID, identical across every BtGattServer-assembled service (when
// CONFIG_APP_BT_METADATA_CHARACTERISTIC is enabled - see fw/Kconfig), exposing every sibling
// characteristic's CUD name + CPF format in one packed blob, read in a single ATT round-trip
// instead of two descriptor reads per characteristic (issue #41 follow-up). Must match
// kMetadataCharacteristicUuid in fw/src/bluetooth/bt_service_cpp.h. Services that don't expose
// this characteristic (e.g. the third-party McuMgr service, or rgb_sunglasses_dk builds with the
// Kconfig disabled) fall back to the per-descriptor read path - see parseMetadataBlob() in
// services/ble-value-codec.ts and the discovery loop in hooks/use-ble-connection.ts.
export const UUID_METADATA_CHARACTERISTIC = "12345678-1234-5678-cccc-56789abd0000";

// Wire-format version for the bulk metadata blob (see parseMetadataBlob()). Must match
// kMetadataBlobVersion in fw/src/bluetooth/bt_service_cpp.h. Bump together if the byte layout
// ever changes; a mismatch is detected and triggers the per-descriptor fallback rather than
// misparsing the blob.
export const METADATA_BLOB_VERSION = 1;

// Base GATT/GAP services every BLE peripheral exposes. Not useful to display in the device
// state UI, so device-state.tsx filters services with these UUIDs out of the render loop.
export const UUID_GENERIC_ATTRIBUTE_SERVICE = "00001801-0000-1000-8000-00805f9b34fb";
export const UUID_GENERIC_ACCESS_SERVICE = "00001800-0000-1000-8000-00805f9b34fb";

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

// Custom format for a drop-down selection list (not standard). Value is a \n-separated string
// of valid options, with the currently-selected option listed first. Write the bare text of one
// of the listed options (no separators) to select it. Must match BLE_GATT_CPF_FORMAT_DROPDOWN_LIST
// in fw/src/bluetooth/gatt_cpf.h.
export const BLE_GATT_CPF_FORMAT_DROPDOWN_LIST = 0xE1;

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