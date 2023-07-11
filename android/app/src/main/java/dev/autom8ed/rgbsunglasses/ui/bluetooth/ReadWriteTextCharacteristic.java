package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;

import androidx.annotation.NonNull;

import java.nio.charset.StandardCharsets;

import dev.autom8ed.rgbsunglasses.ui.bluetooth.DevKitBtInterface;

public class ReadWriteTextCharacteristic {

    EditText editText;
    Button button;
    BluetoothGattCharacteristic myCharacteristic;

    DevKitBtInterface dkInterface;

    Handler handler = new Handler(Looper.getMainLooper());

    BluetoothGattCallback callback = new BluetoothGattCallback() {
        @Override
        public void onCharacteristicRead(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattCharacteristic characteristic, @NonNull byte[] value, int status) {
            super.onCharacteristicRead(gatt, characteristic, value, status);

            if (characteristic.getUuid().equals(myCharacteristic.getUuid())) {
                String str = new String(value, StandardCharsets.UTF_8);
                Log.i("ReadWriteTextCharacteristic", "read complete for " + myCharacteristic.getUuid().toString() + ": " + str);
                handler.post(new Runnable() {
                    @Override
                    public void run() {

                        editText.setText(str);
                    }
                });
            }
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic, int status) {
            super.onCharacteristicWrite(gatt, characteristic, status);

            if (characteristic.getUuid().equals(myCharacteristic.getUuid())) {
                Log.i("ReadWriteTextCharacteristic", "write complete for " + myCharacteristic.getUuid().toString() + ": " + status);
                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        button.setEnabled(true);
                    }
                });
            }
        }
    };

    @SuppressLint("MissingPermission")
    public ReadWriteTextCharacteristic(EditText e, Button b, BluetoothGattCharacteristic c, DevKitBtInterface i) {
        editText = e;
        button = b;
        myCharacteristic = c;
        dkInterface = i;

        // Reset button state
        button.setEnabled(true);

        // Register that we want callbacks for GATT reads
        dkInterface.registerForGattCallback(callback);

        Log.i("ReadWriteTextCharacteristic","Reading characteristic " + myCharacteristic.getUuid().toString());

        // Read the characteristic
        dkInterface.queueReadCharacteristic(myCharacteristic);

        // Wire up callback to button press events to write the characteristic
        button.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                button.setEnabled(false);
                String currentText = String.valueOf(editText.getText());
                dkInterface.queueWriteCharacteristic(myCharacteristic, currentText.getBytes(StandardCharsets.UTF_8), BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            }
        });
    }
}
