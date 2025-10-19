package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;

import androidx.annotation.NonNull;

import java.nio.charset.StandardCharsets;

import dev.autom8ed.rgbsunglasses.databinding.ReadWriteStringBinding;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.DevKitBtInterface;

public class ReadWriteTextCharacteristic extends DeviceGeneratedUiBase {

    BluetoothGattCharacteristic myCharacteristic;

    DevKitBtInterface dkInterface;

    Handler handler = new Handler(Looper.getMainLooper());

    ReadWriteStringBinding binding;

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

                        binding.editSlot0.setText(str);
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
                        binding.submitSlot0.setEnabled(true);
                    }
                });
            }
        }
    };

    BluetoothGattDescriptor myCcc;

    @SuppressLint("MissingPermission")
    public ReadWriteTextCharacteristic(@NonNull LayoutInflater inflater,
                                       LinearLayout layout,
                                       BluetoothGattCharacteristic c,
                                       String cud,
                                       byte cpf,
                                       BluetoothGattDescriptor ccc,
                                       DevKitBtInterface i) {

        binding = ReadWriteStringBinding.inflate(inflater, layout, true);

        myCharacteristic = c;
        dkInterface = i;

        // Reset button state
        binding.submitSlot0.setEnabled(true);
        binding.textView3.setText(cud);

        // Register that we want callbacks for GATT reads
        dkInterface.registerForGattCallback(callback);

        Log.i("ReadWriteTextCharacteristic","Reading characteristic " + myCharacteristic.getUuid().toString());

        // Read the characteristic
        dkInterface.queueReadCharacteristic(myCharacteristic);

        // Wire up callback to button press events to write the characteristic
        binding.submitSlot0.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                binding.submitSlot0.setEnabled(false);
                String currentText = String.valueOf(binding.editSlot0.getText());
                dkInterface.queueWriteCharacteristic(myCharacteristic, currentText.getBytes(StandardCharsets.UTF_8), BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            }
        });
    }
}
