package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import static android.bluetooth.BluetoothProfile.GATT;

import android.Manifest;
import android.annotation.SuppressLint;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothDevice;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.core.app.ActivityCompat;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;

import dev.autom8ed.rgbsunglasses.databinding.FragmentBluetoothscanBinding;

import dev.autom8ed.rgbsunglasses.ui.animations.AnimationType;

import java.util.List;
import java.util.Objects;
import java.util.UUID;

public class BluetoothScanFragment extends Fragment  {

    private FragmentBluetoothscanBinding binding;
    private BluetoothDevice dkDevice = null;
    private BluetoothGatt dkGattDevice = null;

    private BluetoothGattCallback gattCallback = null;



    public AnimationType isAnimationService(UUID uuid) {
        for (AnimationType type : AnimationType.values()) {
            if (uuid.equals(BluetoothHelpers.getUuidForAnimationService(type.ordinal()))) {
                return type;
            }
        }

        return AnimationType.None;
    }

    public void createGattCallback() {
        gattCallback = new BluetoothGattCallback() {
            @Override
            public void onPhyUpdate(BluetoothGatt gatt, int txPhy, int rxPhy, int status) {
                super.onPhyUpdate(gatt, txPhy, rxPhy, status);
                Log.i("Bluetooth", "onPhyUpdate");
            }

            @Override
            public void onPhyRead(BluetoothGatt gatt, int txPhy, int rxPhy, int status) {
                super.onPhyRead(gatt, txPhy, rxPhy, status);
                Log.i("Bluetooth", "onPhyRead");
            }

            @SuppressLint("MissingPermission")
            @Override
            public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
                super.onConnectionStateChange(gatt, status, newState);
                Log.i("Bluetooth", "onConnectionStateChange: status = " +  status + " newState = " + newState);

                if (newState == BluetoothGatt.STATE_CONNECTED) {
                    Log.i("Bluetooth", "Connected!");
                    dkGattDevice = gatt;
                    dkGattDevice.discoverServices();

                } else if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                    Log.i("Bluetooth", "Disconnected");
                    dkGattDevice = null;
                }
            }

            @Override
            public void onServicesDiscovered(BluetoothGatt gatt, int status) {
                super.onServicesDiscovered(gatt, status);
                Log.i("Bluetooth", "onServicesDiscovered");

                List<BluetoothGattService> services = gatt.getServices();
                for (BluetoothGattService service : services) {
                    Log.i("Bluetooth", "Service: " + service.getUuid().toString());
                    AnimationType type = isAnimationService(service.getUuid());
                    if (type != AnimationType.None) {
                        Log.i("Bluetooth", "Found matching Animation Service: " + type.toString());
                        // Pass the characteristic to the View/Model/Fragment for the relevant page?
                    }

                    for (BluetoothGattCharacteristic characteristic : service.getCharacteristics()) {
                        Log.i("Bluetooth", "Characteristic: " + characteristic.getUuid().toString());
                        for (BluetoothGattDescriptor descriptor : characteristic.getDescriptors()) {
                            Log.i("Bluetooth", "Descriptor: " + descriptor.getUuid());
                        }
                    }
                }
            }

            @Override
            public void onCharacteristicRead(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattCharacteristic characteristic, @NonNull byte[] value, int status) {
                super.onCharacteristicRead(gatt, characteristic, value, status);
                Log.i("Bluetooth", "onCharacteristicRead");
            }

            @Override
            public void onCharacteristicWrite(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic, int status) {
                super.onCharacteristicWrite(gatt, characteristic, status);
                Log.i("Bluetooth", "onCharacteristicWrite");
            }

            @Override
            public void onCharacteristicChanged(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattCharacteristic characteristic, @NonNull byte[] value) {
                super.onCharacteristicChanged(gatt, characteristic, value);
                Log.i("Bluetooth", "onCharacteristicChanged");
            }

            @Override
            public void onDescriptorRead(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattDescriptor descriptor, int status, @NonNull byte[] value) {
                super.onDescriptorRead(gatt, descriptor, status, value);
                Log.i("Bluetooth", "onDescriptorRead");
            }

            @Override
            public void onDescriptorWrite(BluetoothGatt gatt, BluetoothGattDescriptor descriptor, int status) {
                super.onDescriptorWrite(gatt, descriptor, status);
                Log.i("Bluetooth", "onDescriptorWrite");
            }

            @Override
            public void onReliableWriteCompleted(BluetoothGatt gatt, int status) {
                super.onReliableWriteCompleted(gatt, status);
                Log.i("Bluetooth", "onReliableWriteCompleted");
            }

            @Override
            public void onReadRemoteRssi(BluetoothGatt gatt, int rssi, int status) {
                super.onReadRemoteRssi(gatt, rssi, status);
                Log.i("Bluetooth", "onReadRemoteRssi");
            }

            @Override
            public void onMtuChanged(BluetoothGatt gatt, int mtu, int status) {
                super.onMtuChanged(gatt, mtu, status);
                Log.i("Bluetooth", "onMtuChanged");
            }

            @Override
            public void onServiceChanged(@NonNull BluetoothGatt gatt) {
                super.onServiceChanged(gatt);
                Log.i("Bluetooth", "onServiceChanged");
            }
        };
    }

    public View onCreateView(@NonNull LayoutInflater inflater,
                             ViewGroup container, Bundle savedInstanceState) {
        BluetoothScanViewModel btScanViewModel =
                new ViewModelProvider(this).get(BluetoothScanViewModel.class);

        binding = FragmentBluetoothscanBinding.inflate(inflater, container, false);
        View root = binding.getRoot();

        final TextView textView = binding.textBluetoothscan;
        btScanViewModel.getText().observe(getViewLifecycleOwner(), textView::setText);

        BluetoothManager manager = (BluetoothManager) getContext().getSystemService(Context.BLUETOOTH_SERVICE);
        if (ActivityCompat.checkSelfPermission(getContext(), Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            // TODO: Consider calling
            //    ActivityCompat#requestPermissions
            // here to request the missing permissions, and then overriding
            //   public void onRequestPermissionsResult(int requestCode, String[] permissions,
            //                                          int[] grantResults)
            // to handle the case where the user grants the permission. See the documentation
            // for ActivityCompat#requestPermissions for more details.
            return root;
        }
        List<BluetoothDevice> connectedDevices = manager.getConnectedDevices(GATT);

        createGattCallback();

        btScanViewModel.setText("Not connected to devkit");

        for (BluetoothDevice dev : connectedDevices) {
            Log.i("Bluetooth", "Connected to device: " + dev.getName());

            if (Objects.equals(dev.getName(), "RGB Sunglasses DK")) {
                dkDevice = dev;
                dev.connectGatt(getContext(), true, gattCallback);
                btScanViewModel.setText("Connected to devkit!");
            }
        }

        return root;
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;
    }
}
