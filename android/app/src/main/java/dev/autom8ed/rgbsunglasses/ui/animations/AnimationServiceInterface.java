package dev.autom8ed.rgbsunglasses.ui.animations;

import android.bluetooth.BluetoothGattService;

public abstract class AnimationServiceInterface {
    /**
     * Called when the desired GATT Service was located
     */
    public void onGattServiceFound(BluetoothGattService service) {
    }
}
