package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.CompoundButton;
import android.widget.Switch;

import androidx.annotation.NonNull;

import java.nio.charset.StandardCharsets;
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

    BluetoothGattCharacteristic myCharacteristic;
    Switch sw;

    DevKitBtInterface dkInterface;

    BluetoothGattCallback callback = new BluetoothGattCallback() {
        @Override
        public void onCharacteristicRead(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattCharacteristic characteristic, @NonNull byte[] value, int status) {
            super.onCharacteristicRead(gatt, characteristic, value, status);

            if (characteristic.getUuid().equals(myCharacteristic.getUuid())) {
                Log.i("IsActiveCharacteristic", "read complete for " + myCharacteristic.getUuid().toString() + ": " + value[0]);
                boolean state = value[0] == 1;

                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        sw.setChecked(state);
                    }
                });
            }
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic, int status) {
            super.onCharacteristicWrite(gatt, characteristic, status);

            if (characteristic.getUuid().equals(myCharacteristic.getUuid())) {
                Log.i("IsActiveCharacteristic", "write complete for " + myCharacteristic.getUuid().toString() + ": " + status);
                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        sw.setEnabled(true);
                    }
                });
            }
        }

        @Override
        public void onCharacteristicChanged(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattCharacteristic characteristic, @NonNull byte[] value) {
            super.onCharacteristicChanged(gatt, characteristic, value);
            Log.i("IsActiveCharacteristic", "onCharacteristicChanged");
        }
    };

    Handler handler = new Handler(Looper.getMainLooper());

    @SuppressLint("MissingPermission")
    public IsActiveCharacteristic(BluetoothGattCharacteristic c, Switch s, DevKitBtInterface i) {

        myCharacteristic = c;
        sw = s;
        dkInterface = i;

        // Reset sw state
        sw.setEnabled(true);

        // Register that we want callbacks for GATT reads
        dkInterface.registerForGattCallback(callback);

        // Read the characteristic
        dkInterface.queueReadCharacteristic(myCharacteristic);

        // Enable characteristic notification
        if (!dkInterface.dkGattDevice.setCharacteristicNotification(myCharacteristic, true)) {
            Log.e("IsActiveCharacteristic", "Failed to register for characteristic notification");
        }

        sw.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton compoundButton, boolean b) {
                byte[] v = new byte[1];
                v[0] = (byte) (b ? 1 : 0);
                dkInterface.queueWriteCharacteristic(myCharacteristic, v, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            }
        });
    }
}
