package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import android.bluetooth.BluetoothGattCharacteristic;

public class BluetoothCharacteristicWriteQueueEntry {
    public BluetoothGattCharacteristic characteristic;
    public byte[] value;
    public int writeType;
}
