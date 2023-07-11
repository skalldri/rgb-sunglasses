package dev.autom8ed.rgbsunglasses.ui.animations;
import static dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothHelpers.getUuidForCpfDescriptor;
import static dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothHelpers.getUuidForCudDescriptor;

import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothStatusCodes;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Switch;

import androidx.annotation.NonNull;
import androidx.fragment.app.Fragment;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

import dev.autom8ed.rgbsunglasses.databinding.FragmentZigzaganimationBinding;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothDescriptorInfo;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothHelpers;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.DevKitBtInterface;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.IsActiveCharacteristic;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.ReadWriteIntegerCharacteristic;

public class AnimationBoilerplate extends Fragment {

    // Could be turned into a subclass
    Handler handler = new Handler(Looper.getMainLooper());

    LayoutInflater myInflater;

    IsActiveCharacteristic isActiveCharacteristic = null;
    DevKitBtInterface dkInterface = null;

    AnimationServiceInterface animServiceCallback = new AnimationServiceInterface() {
        @Override
        public void onGattServiceFound(BluetoothGattService service) {
            super.onGattServiceFound(service);

            for (BluetoothGattCharacteristic characteristic : service.getCharacteristics()) {
                if(IsActiveCharacteristic.isIsActiveCharacteristic(characteristic.getUuid(), kAnimationType.ordinal())) {
                    Log.i("ZigZagAnimation", "Found IsActive characteristic!");
                    //isActiveCharacteristic = new IsActiveCharacteristic(characteristic, isActiveSw, dkInterface);
                    continue;
                } else {
                    Log.i("ZigZagAnimation", "Got unknown char " + characteristic.getUuid().toString());

                    // Lets figure out more about this characteristic
                    for (BluetoothGattDescriptor d : characteristic.getDescriptors()) {
                        Log.i("ZigZagAnimation", "Reading descriptor: " + d.getUuid().toString());
                        dkInterface.queueReadDescriptor(d);
                    }
                }

            }
        }
    };

    Map<BluetoothGattCharacteristic, List<BluetoothDescriptorInfo>> chrcToDescMap = new ConcurrentHashMap<>();

    List<ReadWriteIntegerCharacteristic> myRwIntChrcs = new ArrayList<>();

    BluetoothGattCallback callback = new BluetoothGattCallback() {
        @Override
        public void onDescriptorRead(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattDescriptor descriptor, int status, @NonNull byte[] value) {
            super.onDescriptorRead(gatt, descriptor, status, value);

            Log.i("ZigZagAnim", "onDescriptorRead: " + descriptor.getUuid().toString() + " status = " +  status);

            if (status == BluetoothStatusCodes.SUCCESS) {
                if (!chrcToDescMap.containsKey(descriptor.getCharacteristic())) {
                    // We have not seen this characteristic yet, insert an empty arraylist into the map
                    chrcToDescMap.put(descriptor.getCharacteristic(), new ArrayList<BluetoothDescriptorInfo>());
                    Log.i("ZigZagAnim", "Creating new map list");
                }

                BluetoothDescriptorInfo info = new BluetoothDescriptorInfo();
                info.descriptor = descriptor;
                info.value = value;

                // Add descriptor to the list
                Log.i("ZigZagAnim", "Adding");
                chrcToDescMap.get(descriptor.getCharacteristic()).add(info);
            }

            Log.i("ZigZagAnim", "List Len: " + chrcToDescMap.get(descriptor.getCharacteristic()).size());

            BluetoothDescriptorInfo haveCpf = null;
            BluetoothDescriptorInfo haveCud = null;
            for (BluetoothDescriptorInfo i : chrcToDescMap.get(descriptor.getCharacteristic())) {
                Log.i("ZigZagAnim", "Testing descriptor " + i.descriptor.getUuid().toString());
                if (i.descriptor.getUuid().equals(getUuidForCpfDescriptor())) {
                    Log.i("ZigZagAnim", "Have CPF!");
                    haveCpf = i;
                } else if (i.descriptor.getUuid().equals(getUuidForCudDescriptor())) {
                    Log.i("ZigZagAnim", "Have CUD!");
                    haveCud = i;
                }
            }

            if (haveCpf != null && haveCud != null) {
                Log.i("ZigZagAnim",  "Format: " + haveCpf.value[0]);
                String cud = new String(haveCud.value, StandardCharsets.UTF_8);
                Log.i("ZigZagAnim",  "Description: " + cud);

                byte cpf = haveCpf.value[0];
                if (cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT8 ||
                        cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT16 ||
                        cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT32 ||
                        cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_SINT8 ||
                        cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_SINT16 ||
                        cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_SINT32) {
                    // Create a new ReadWriteIntegerCharacteristic and add it to the display
                    handler.post(new Runnable() {
                        @Override
                        public void run() {
                            /*ReadWriteIntegerCharacteristic rwIntChrc = new ReadWriteIntegerCharacteristic(
                                    myInflater,
                                    binding.linearLayoutVertical,
                                    descriptor.getCharacteristic(),
                                    cud,
                                    cpf,
                                    dkInterface);
                            myRwIntChrcs.add(rwIntChrc);*/
                        }
                    });
                }


            }
        }
    };

    /*
    public View onCreateView(@NonNull LayoutInflater inflater,
                             ViewGroup container, Bundle savedInstanceState) {
        myInflater = inflater;

        // binding = FragmentZigzaganimationBinding.inflate(inflater, container, false);
        // View root = binding.getRoot();

        Log.i("ZigZagAnimation", "onCreateView");

        dkInterface = new DevKitBtInterface(getContext(), animServiceCallback, kAnimationType);

        dkInterface.registerForGattCallback(callback);

        return root;
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        //binding = null;

        Log.i("ZigZagAnimation", "onDestroyView");
    }
    */

    AnimationType kAnimationType;

    public AnimationBoilerplate(AnimationType a) {
        kAnimationType = a;
    }
}
