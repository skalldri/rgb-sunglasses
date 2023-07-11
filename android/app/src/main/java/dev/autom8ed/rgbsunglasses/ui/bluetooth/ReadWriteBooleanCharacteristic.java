package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.LayoutInflater;
import android.widget.CompoundButton;
import android.widget.LinearLayout;

import androidx.annotation.NonNull;

import dev.autom8ed.rgbsunglasses.databinding.ReadWriteIntBinding;
import dev.autom8ed.rgbsunglasses.databinding.ReadWriteNotifBoolBinding;

public class ReadWriteBooleanCharacteristic extends DeviceGeneratedUiBase {
    private @NonNull ReadWriteNotifBoolBinding binding;

    BluetoothGattCharacteristic myCharacteristic;

    DevKitBtInterface dkInterface;

    Handler handler = new Handler(Looper.getMainLooper());

    BluetoothGattCallback callback = new BluetoothGattCallback() {
        @Override
        public void onCharacteristicRead(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattCharacteristic characteristic, @NonNull byte[] value, int status) {
            super.onCharacteristicRead(gatt, characteristic, value, status);

            if (characteristic.getUuid().equals(myCharacteristic.getUuid())) {
                // String str = new String(value, StandardCharsets.UTF_8);
                Log.i("ReadWriteBooleanCharacteristic", "read complete for " + myCharacteristic.getUuid().toString() + ": ");
                boolean state = value[0] == 1;

                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        binding.switch1.setChecked(state);
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
                        binding.switch1.setEnabled(true);
                    }
                });
            }
        }
    };

    byte myCpf;

    @SuppressLint("MissingPermission")
    public ReadWriteBooleanCharacteristic(
            @NonNull LayoutInflater inflater,
            LinearLayout layout,
            BluetoothGattCharacteristic c,
            String cud,
            byte cpf,
            DevKitBtInterface i) {
        binding = ReadWriteNotifBoolBinding.inflate(inflater, layout, true);

        dkInterface = i;
        myCharacteristic = c;
        myCpf = cpf;

        // Reset button state
        binding.switch1.setEnabled(true);
        binding.switch1.setText(cud);

        // Register that we want callbacks for GATT reads
        dkInterface.registerForGattCallback(callback);

        Log.i("ReadWriteIntegerCharacteristic","Reading characteristic " + myCharacteristic.getUuid().toString());

        // Read the characteristic
        dkInterface.queueReadCharacteristic(myCharacteristic);

        // Enable characteristic notification
        if (!dkInterface.dkGattDevice.setCharacteristicNotification(myCharacteristic, true)) {
            Log.e("ReadWriteBoolean", "Failed to register for characteristic notification");
        }

        binding.switch1.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton compoundButton, boolean b) {
                byte[] v = new byte[1];
                v[0] = (byte) (b ? 1 : 0);
                dkInterface.queueWriteCharacteristic(myCharacteristic, v, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            }
        });
    }
}
