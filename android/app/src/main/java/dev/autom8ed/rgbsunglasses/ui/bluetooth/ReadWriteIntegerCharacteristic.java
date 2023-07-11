package dev.autom8ed.rgbsunglasses.ui.bluetooth;

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

import dev.autom8ed.rgbsunglasses.databinding.ReadWriteIntBinding;

public class ReadWriteIntegerCharacteristic extends DeviceGeneratedUiBase {
    private @NonNull ReadWriteIntBinding binding;

    BluetoothGattCharacteristic myCharacteristic;

    DevKitBtInterface dkInterface;

    Handler handler = new Handler(Looper.getMainLooper());

    long bytesToInt(byte[] value) {
        if (myCpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT8) {
            return ((int)value[0]) & 0xFF;
        }
        else if (myCpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT16) {
            return ((value[1] << 8) | value[0]) & 0xFFFF;
        }
        else if (myCpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT32) {
            return ((value[3] << 24) | (value[2] << 16) | (value[1] << 8) | value[0]) & 0xFFFFFFFF;
        }
        return 12345;
    }

    byte[] strToBytes(String value) {
        byte[] v = new byte[1];
        v[0] = 0;

        if (myCpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT8) {
            v = new byte[1];
            v[0] = Byte.parseByte(value);
        }
        else if (myCpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT16) {
            v = new byte[2];
            long l = Long.parseLong(value);
            v[0] = (byte) (l & 0xFF);
            v[1] = (byte) ((l >> 8) & 0xFF);
        }
        else if (myCpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT32) {
            v = new byte[4];
            long l = Long.parseLong(value);
            v[0] = (byte) (l & 0xFF);
            v[1] = (byte) ((l >> 8) & 0xFF);
            v[2] = (byte) ((l >> 16) & 0xFF);
            v[3] = (byte) ((l >> 24) & 0xFF);
        }
        return v;
    }

    BluetoothGattCallback callback = new BluetoothGattCallback() {
        @Override
        public void onCharacteristicRead(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattCharacteristic characteristic, @NonNull byte[] value, int status) {
            super.onCharacteristicRead(gatt, characteristic, value, status);

            if (characteristic.getUuid().equals(myCharacteristic.getUuid())) {
                // String str = new String(value, StandardCharsets.UTF_8);
                Log.i("ReadWriteTextCharacteristic", "read complete for " + myCharacteristic.getUuid().toString() + ": ");
                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        binding.editTextNumber.setText(String.valueOf(bytesToInt(value)));
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
                        binding.button.setEnabled(true);
                    }
                });
            }
        }
    };

    byte myCpf;

    public ReadWriteIntegerCharacteristic(
            @NonNull LayoutInflater inflater,
            LinearLayout layout,
            BluetoothGattCharacteristic c,
            String cud,
            byte cpf,
            DevKitBtInterface i) {
        binding = ReadWriteIntBinding.inflate(inflater, layout, true);

        dkInterface = i;
        myCharacteristic = c;
        myCpf = cpf;

        // Reset button state
        binding.button.setEnabled(true);
        binding.textView2.setText(cud);

        // Register that we want callbacks for GATT reads
        dkInterface.registerForGattCallback(callback);

        Log.i("ReadWriteIntegerCharacteristic","Reading characteristic " + myCharacteristic.getUuid().toString());

        // Read the characteristic
        dkInterface.queueReadCharacteristic(myCharacteristic);

        binding.button.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                binding.button.setEnabled(false);
                String currentText = String.valueOf(binding.editTextNumber.getText());
                dkInterface.queueWriteCharacteristic(myCharacteristic, strToBytes(currentText), BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            }
        });
    }
}
