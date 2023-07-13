package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import android.bluetooth.BluetoothGattDescriptor;

public class BluetoothDescriptorWriteQueueEntry {
    public BluetoothGattDescriptor descriptor;
    public byte[] value;
    public int writeType;
}

