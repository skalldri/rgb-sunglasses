package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.LayoutInflater;
import android.widget.LinearLayout;

import androidx.annotation.NonNull;

import com.skydoves.colorpickerview.listeners.ColorListener;

import dev.autom8ed.rgbsunglasses.databinding.ColorPickerBinding;
import dev.autom8ed.rgbsunglasses.databinding.ReadWriteIntBinding;

public class ReadWriteIntegerColorCharacteristic extends DeviceGeneratedUiBase {

    private @NonNull ColorPickerBinding binding;

    BluetoothGattCharacteristic myCharacteristic;

    DevKitBtInterface dkInterface;

    Handler handler = new Handler(Looper.getMainLooper());

    byte myCpf;
    BluetoothGattDescriptor myCcc;

    public byte[] intToBytes(int value) {
        byte[] v = new byte[1];
        v[0] = 0;

        if (myCpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT32) {
            v = new byte[4];
            v[0] = (byte) (value & 0xFF);
            v[1] = (byte) ((value >> 8) & 0xFF);
            v[2] = (byte) ((value >> 16) & 0xFF);
            v[3] = (byte) ((value >> 24) & 0xFF);
        }
        return v;
    }

    public long bytesToInt(byte[] value) {
        if (myCpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT8) {
            return ((int)value[0]) & 0xFF;
        }
        else if (myCpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT16) {
            return (((value[1] & 0xFF) << 8) | (value[0] & 0xFF)) & 0xFFFF;
        }
        else if (myCpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT32) {
            return (((value[3] & 0xFF) << 24) | ((value[2] & 0xFF) << 16) | ((value[1] &0xFF) << 8) | (value[0] & 0xFF)) & 0xFFFFFFFF;
        }
        return 12345;
    }

    BluetoothGattCallback callback = new BluetoothGattCallback() {
        @Override
        public void onCharacteristicRead(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattCharacteristic characteristic, @NonNull byte[] value, int status) {
            super.onCharacteristicRead(gatt, characteristic, value, status);

            if (characteristic.getUuid().equals(myCharacteristic.getUuid())) {
                // String str = new String(value, StandardCharsets.UTF_8);
                Log.i("ReadWriteIntegerColorCharacteristic", "read complete for " + myCharacteristic.getUuid().toString() + ": ");
                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        // binding.colorPickerView.setColor(String.valueOf(bytesToInt(value)));
                    }
                });
            }
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic, int status) {
            super.onCharacteristicWrite(gatt, characteristic, status);

            if (characteristic.getUuid().equals(myCharacteristic.getUuid())) {
                Log.i("ReadWriteIntegerCharacteristic", "write complete for " + myCharacteristic.getUuid().toString() + ": " + status);
                handler.post(new Runnable() {
                    @Override
                    public void run() {

                        // binding.button.setEnabled(true);
                    }
                });
            }
        }

        @Override
        public void onCharacteristicChanged(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattCharacteristic characteristic, @NonNull byte[] value) {
            super.onCharacteristicChanged(gatt, characteristic, value);
            Log.i("ReadWriteIntegerCharacteristic", "onCharacteristicChanged");

            if (characteristic.getUuid().equals(myCharacteristic.getUuid())) {
                // String str = new String(value, StandardCharsets.UTF_8);
                Log.i("ReadWriteIntegerCharacteristic", "read complete for " + myCharacteristic.getUuid().toString() + ": ");
                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        // binding.editTextNumber.setText(String.valueOf(bytesToInt(value)));
                    }
                });
            }
        }
    };

    public ReadWriteIntegerColorCharacteristic(@NonNull LayoutInflater inflater, LinearLayout layout, BluetoothGattCharacteristic c, String cud, byte cpf, BluetoothGattDescriptor ccc, DevKitBtInterface i) {
        binding = ColorPickerBinding.inflate(inflater, layout, true);

        dkInterface = i;
        myCharacteristic = c;
        myCpf = cpf;
        myCcc = ccc;

        binding.colorPickerView.setColorListener(new ColorListener() {
            @Override
            public void onColorSelected(int color, boolean fromUser) {
                dkInterface.queueWriteCharacteristic(myCharacteristic, intToBytes(color), BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            }
        });
    }
}
