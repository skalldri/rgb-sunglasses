package dev.autom8ed.rgbsunglasses.ui.bluetooth;

import static android.bluetooth.BluetoothProfile.GATT;

import android.Manifest;
import android.annotation.SuppressLint;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothStatusCodes;
import android.content.Context;
import android.content.pm.PackageManager;
import android.util.Log;

import androidx.annotation.NonNull;
import androidx.core.app.ActivityCompat;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.ConcurrentLinkedQueue;

import dev.autom8ed.rgbsunglasses.ui.animations.AnimationServiceInterface;
import dev.autom8ed.rgbsunglasses.ui.animations.BtServiceType;



public class DevKitBtInterface {

    android.content.Context context = null;

    List<BluetoothGattCallback> extraCallbacks = new ArrayList<>();

    private BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        public void onDescriptorRead(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattDescriptor descriptor, int status, @NonNull byte[] value) {
            super.onDescriptorRead(gatt, descriptor, status, value);

            Log.i("DevKitBtInterface", "onDescriptorRead: " + descriptor.getUuid().toString() + " status = " +  status);

            for (BluetoothGattCallback c : extraCallbacks) {
                c.onDescriptorRead(gatt, descriptor, status, value);
            }

            serviceQueuedActions();
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt gatt, BluetoothGattDescriptor descriptor, int status) {
            super.onDescriptorWrite(gatt, descriptor, status);

            for (BluetoothGattCallback c : extraCallbacks) {
                c.onDescriptorWrite(gatt, descriptor, status);
            }

            serviceQueuedActions();
        }

        @SuppressLint("MissingPermission")
        @Override
        public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
            super.onConnectionStateChange(gatt, status, newState);
            Log.i("DevKitBtInterface", "onConnectionStateChange: status = " +  status + " newState = " + newState);

            if (newState == BluetoothGatt.STATE_CONNECTED) {
                Log.i("Bluetooth", "Connected!");
                dkGattDevice = gatt;
                dkGattDevice.discoverServices();

            } else if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                Log.i("DevKitBtInterface", "Disconnected");
                dkGattDevice = null;
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
            super.onServicesDiscovered(gatt, status);
            Log.i("DevKitBtInterface", "onServicesDiscovered");

            dkGattServices = gatt.getServices();
            for (BluetoothGattService service : dkGattServices) {
                Log.d("DevKitBtInterface", "Service: " + service.getUuid().toString());
                BtServiceType type = BluetoothHelpers.isAnimationService(service.getUuid());
                if (type == animType) {
                    Log.i("DevKitBtInterface", "Found matching Animation Service: " + type.toString());
                    animServiceCallback.onGattServiceFound(service);

                        /*
                        for (BluetoothGattCharacteristic characteristic : service.getCharacteristics()) {
                            Log.d("DevKitBtInterface", "Characteristic: " + characteristic.getUuid().toString());
                            for (BluetoothGattDescriptor descriptor : characteristic.getDescriptors()) {
                                Log.d("DevKitBtInterface", "Descriptor: " + descriptor.getUuid());
                            }
                        }
                         */
                }
            }

            serviceQueuedActions();
        }

        @Override
        public void onCharacteristicRead(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattCharacteristic characteristic, @NonNull byte[] value, int status) {
            super.onCharacteristicRead(gatt, characteristic, value, status);
            Log.i("DevKitBtInterface", "onCharacteristicRead");

            for (BluetoothGattCallback c : extraCallbacks) {
                c.onCharacteristicRead(gatt, characteristic, value, status);
            }

            serviceQueuedActions();
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic, int status) {
            super.onCharacteristicWrite(gatt, characteristic, status);
            Log.i("DevKitBtInterface", "onCharacteristicWrite");

            for (BluetoothGattCallback c : extraCallbacks) {
                c.onCharacteristicWrite(gatt, characteristic, status);
            }

            serviceQueuedActions();
        }

        @Override
        public void onCharacteristicChanged(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattCharacteristic characteristic, @NonNull byte[] value) {
            super.onCharacteristicChanged(gatt, characteristic, value);
            Log.i("DevKitBtInterface", "onCharacteristicChanged");

            for (BluetoothGattCallback c : extraCallbacks) {
                c.onCharacteristicChanged(gatt, characteristic, value);
            }

            serviceQueuedActions();
        }
    };

    private BluetoothDevice dkDevice = null;
    public BluetoothGatt dkGattDevice = null;

    List<BluetoothGattService> dkGattServices;

    AnimationServiceInterface animServiceCallback = null;

    BtServiceType animType;

    @SuppressLint("MissingPermission")
    private void serviceQueuedActions() {
        if (queuedCharacteristicWrites.size() != 0) {
            BluetoothCharacteristicWriteQueueEntry c = queuedCharacteristicWrites.peek();

            if (dkGattDevice.writeCharacteristic(c.characteristic, c.value, c.writeType) == BluetoothStatusCodes.SUCCESS) {
                // Only remove from the queue if we successfully scheduled a read
                Log.i("DevKitBtInterface", "Successfully started next queued characteristic write for " + c.characteristic.getUuid().toString());
                queuedCharacteristicWrites.remove();
            } else {
                Log.i("DevKitBtInterface", "Re-queuing write for characteristic " + c.characteristic.getUuid().toString());
            }
        }

        if (queuedDescriptorWrites.size() != 0) {
            BluetoothDescriptorWriteQueueEntry c = queuedDescriptorWrites.peek();

            if (dkGattDevice.writeDescriptor(c.descriptor, c.value) == BluetoothStatusCodes.SUCCESS) {
                // Only remove from the queue if we successfully scheduled a read
                Log.i("DevKitBtInterface", "Successfully started next queued descriptor write for " + c.descriptor.getUuid().toString());
                queuedDescriptorWrites.remove();
            } else {
                Log.i("DevKitBtInterface", "Re-queuing write for descriptor " + c.descriptor.getUuid().toString());
            }
        }

        if (queuedDescriptorReads.size() != 0) {
            BluetoothGattDescriptor d = queuedDescriptorReads.peek();

            if (dkGattDevice.readDescriptor(d)) {
                // Only remove from the queue if we successfully scheduled a read
                Log.i("DevKitBtInterface", "Successfully started next queued descriptor read for " + d.getUuid().toString());
                queuedDescriptorReads.remove();
            } else {
                Log.i("DevKitBtInterface", "Re-queuing read for descriptor " + d.getUuid().toString());
            }
        }

        if (queuedCharacteristicReads.size() != 0) {
            BluetoothGattCharacteristic c = queuedCharacteristicReads.peek();

            if (dkGattDevice.readCharacteristic(c)) {
                // Only remove from the queue if we successfully scheduled a read
                Log.i("DevKitBtInterface", "Successfully started next queued characteristic read for " + c.getUuid().toString());
                queuedCharacteristicReads.remove();
            } else {
                Log.i("DevKitBtInterface", "Re-queuing read for characteristic " + c.getUuid().toString());
            }
        }
    }

    public DevKitBtInterface(android.content.Context c, AnimationServiceInterface a, BtServiceType t) {
        context = c;
        animServiceCallback = a;
        animType = t;

        BluetoothManager manager = (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        if (ActivityCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            Log.i("DevKitBtInterface","No bluetooth permissions");
            return;
        }
        List<BluetoothDevice> connectedDevices = manager.getConnectedDevices(GATT);

        dkDevice = null;

        for (BluetoothDevice dev : connectedDevices) {
            Log.i("DevKitBtInterface", "Connected to device: " + dev.getName());

            // Try to find the DK
            if (Objects.equals(dev.getName(), "RGB Sunglasses DK")) {
                dkDevice = dev;
                dev.connectGatt(context, true, gattCallback);

                // If we are somehow connected to multiple devices with this name, pick the first one
                break;
            }
        }

        if (dkDevice == null) {
            Log.e("DevKitBtInterface", "Failed to connect to GATT device");
        }
    }

    public void registerForGattCallback(BluetoothGattCallback c) {
        extraCallbacks.add(c);
    }

    public BluetoothGattService findAnimationService(BtServiceType t) {
        if (dkGattServices == null) {
            return null;
        }

        for (BluetoothGattService service : dkGattServices) {
            BtServiceType type = BluetoothHelpers.isAnimationService(service.getUuid());
            if (type == t) {
                Log.i("DevKitBtInterface", "Found matching Animation Service for " + t.toString());
                return service;
            }
        }

        Log.e("DevKitBtInterface", "No matching service for " + t.toString());
        return null;
    }

    private ConcurrentLinkedQueue<BluetoothGattCharacteristic> queuedCharacteristicReads = new ConcurrentLinkedQueue<>();

    @SuppressLint("MissingPermission")
    public void queueReadCharacteristic(BluetoothGattCharacteristic c) {
        // Try and immediately service the request
        if (!dkGattDevice.readCharacteristic(c)) {
            // Could not start the characteristic read: add it to the queue
            Log.i("DevKitBtInterface", "Queued char read for " + c.getUuid().toString());
            queuedCharacteristicReads.add(c);
        } else {
            Log.i("DevKitBtInterface", "Sent char read for " + c.getUuid().toString());
        }
    }

    private ConcurrentLinkedQueue<BluetoothCharacteristicWriteQueueEntry> queuedCharacteristicWrites = new ConcurrentLinkedQueue<>();

    @SuppressLint("MissingPermission")
    public int queueWriteCharacteristic(BluetoothGattCharacteristic c, byte[] value, int writeType) {
        int res = dkGattDevice.writeCharacteristic(c, value, writeType);

        // Try and immediately service the request
        if (res != BluetoothStatusCodes.SUCCESS) {
            // Could not start the characteristic write: add it to the queue
            Log.i("DevKitBtInterface", "Queued char write for " + c.getUuid().toString());
            BluetoothCharacteristicWriteQueueEntry e = new BluetoothCharacteristicWriteQueueEntry();
            e.characteristic = c;
            e.value = value;
            e.writeType = writeType;
            queuedCharacteristicWrites.add(e);
        } else {
            Log.i("DevKitBtInterface", "Sent char write for " + c.getUuid().toString());
        }

        return BluetoothStatusCodes.SUCCESS;
    }

    private ConcurrentLinkedQueue<BluetoothDescriptorWriteQueueEntry> queuedDescriptorWrites = new ConcurrentLinkedQueue<>();

    @SuppressLint("MissingPermission")
    public int queueWriteDescriptor(BluetoothGattDescriptor d, byte[] value) {
        int res = dkGattDevice.writeDescriptor(d, value);

        // Try and immediately service the request
        if (res != BluetoothStatusCodes.SUCCESS) {
            // Could not start the characteristic write: add it to the queue
            Log.i("DevKitBtInterface", "Queued desc write for " + d.getUuid().toString());
            BluetoothDescriptorWriteQueueEntry e = new BluetoothDescriptorWriteQueueEntry();
            e.descriptor = d;
            e.value = value;
            queuedDescriptorWrites.add(e);
        } else {
            Log.i("DevKitBtInterface", "Sent char write for " + d.getUuid().toString());
        }

        return BluetoothStatusCodes.SUCCESS;
    }

    private ConcurrentLinkedQueue<BluetoothGattDescriptor> queuedDescriptorReads = new ConcurrentLinkedQueue<>();

    @SuppressLint("MissingPermission")
    public void queueReadDescriptor(BluetoothGattDescriptor d) {
        // Try and immediately service the request
        if (!dkGattDevice.readDescriptor(d)) {
            // Could not start the characteristic read: add it to the queue
            Log.i("DevKitBtInterface", "Queued desc read for " + d.getUuid().toString());
            queuedDescriptorReads.add(d);
        } else {
            Log.i("DevKitBtInterface", "Sent desc read for " + d.getUuid().toString());
        }
    }
}
