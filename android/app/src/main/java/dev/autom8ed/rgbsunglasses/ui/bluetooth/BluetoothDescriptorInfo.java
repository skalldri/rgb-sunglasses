package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattDescriptor;

public class BluetoothDescriptorInfo {
    public BluetoothGattDescriptor descriptor;
    public byte[] value;
    boolean isProcessed = false;
}
