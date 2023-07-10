package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import android.bluetooth.BluetoothGattCharacteristic;

import java.util.UUID;

public class IsActiveCharacteristic {
    public static UUID getUuidForIsActiveCharacteristic(long animationId) {
        UUID uuid = UUID.fromString(String.format("12345678-1234-5678-%04X-56789abcf002", animationId));
        return uuid;
    }

    public static boolean isIsActiveCharacteristic(UUID uuid, long animationId) {
        if (uuid.equals(getUuidForIsActiveCharacteristic(animationId))) {
            return true;
        }

        return false;
    }

    BluetoothGattCharacteristic characteristic;

    public IsActiveCharacteristic(BluetoothGattCharacteristic c) {
        characteristic = c;
    }
}
