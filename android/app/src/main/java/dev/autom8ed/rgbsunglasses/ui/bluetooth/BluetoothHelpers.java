package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import java.util.UUID;

import dev.autom8ed.rgbsunglasses.ui.animations.BtServiceType;

public class BluetoothHelpers {

    public static final byte BLE_GATT_CPF_FORMAT_RFU =   0x00;
    public static final byte BLE_GATT_CPF_FORMAT_BOOLEAN =   0x01;
    public static final byte BLE_GATT_CPF_FORMAT_2BIT =   0x02;
    public static final byte BLE_GATT_CPF_FORMAT_NIBBLE =   0x03;
    public static final byte BLE_GATT_CPF_FORMAT_UINT8 =   0x04;
    public static final byte BLE_GATT_CPF_FORMAT_UINT12 =   0x05;
    public static final byte BLE_GATT_CPF_FORMAT_UINT16 =   0x06;
    public static final byte BLE_GATT_CPF_FORMAT_UINT24 =   0x07;
    public static final byte BLE_GATT_CPF_FORMAT_UINT32 =   0x08;
    public static final byte BLE_GATT_CPF_FORMAT_UINT48 =   0x09;
    public static final byte BLE_GATT_CPF_FORMAT_UINT64 =   0x0A;
    public static final byte BLE_GATT_CPF_FORMAT_UINT128 =   0x0B;
    public static final byte BLE_GATT_CPF_FORMAT_SINT8 =   0x0C;
    public static final byte BLE_GATT_CPF_FORMAT_SINT12 =   0x0D;
    public static final byte BLE_GATT_CPF_FORMAT_SINT16 =   0x0E;
    public static final byte BLE_GATT_CPF_FORMAT_SINT24 =   0x0F;
    public static final byte BLE_GATT_CPF_FORMAT_SINT32 =   0x10;
    public static final byte BLE_GATT_CPF_FORMAT_SINT48 =   0x11;
    public static final byte BLE_GATT_CPF_FORMAT_SINT64 =   0x12;
    public static final byte BLE_GATT_CPF_FORMAT_SINT128 =   0x13;
    public static final byte BLE_GATT_CPF_FORMAT_FLOAT32 =   0x14;
    public static final byte BLE_GATT_CPF_FORMAT_FLOAT64 =   0x15;
    public static final byte BLE_GATT_CPF_FORMAT_SFLOAT =   0x16;
    public static final byte BLE_GATT_CPF_FORMAT_FLOAT =   0x17;
    public static final byte BLE_GATT_CPF_FORMAT_DUINT16 =   0x18;
    public static final byte BLE_GATT_CPF_FORMAT_UTF8S =   0x19;
    public static final byte BLE_GATT_CPF_FORMAT_UTF16S =   0x1A;
    public static final byte BLE_GATT_CPF_FORMAT_STRUCT =   0x1B;

    public static UUID getUuidForAnimationService(long animationId) {
        UUID uuid = UUID.fromString(String.format("12345678-1234-5678-%04X-56789abc0000", animationId));
        return uuid;
    }

    public static UUID getUuidForCpfDescriptor() {
        return UUID.fromString("00002904-0000-1000-8000-00805f9b34fb");

    }

    public static UUID getUuidForCudDescriptor() {
        return UUID.fromString("00002901-0000-1000-8000-00805f9b34fb");
    }

    public static UUID getUuidForCccDescriptor() {
        return UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    }

    public static BtServiceType isAnimationService(UUID uuid) {
        for (BtServiceType type : BtServiceType.values()) {
            if (uuid.equals(BluetoothHelpers.getUuidForAnimationService(type.ordinal()))) {
                return type;
            }
        }

        return BtServiceType.None;
    }
}
