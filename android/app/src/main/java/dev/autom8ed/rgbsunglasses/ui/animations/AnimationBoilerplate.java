package dev.autom8ed.rgbsunglasses.ui.animations;
import static dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothHelpers.getUuidForCccDescriptor;
import static dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothHelpers.getUuidForCpfDescriptor;
import static dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothHelpers.getUuidForCudDescriptor;

import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothStatusCodes;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.LayoutInflater;
import android.widget.LinearLayout;

import androidx.annotation.NonNull;
import androidx.fragment.app.Fragment;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

import dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothDescriptorInfo;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothGattCharacteristicInfo;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.BluetoothHelpers;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.DevKitBtInterface;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.DeviceGeneratedUiBase;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.ReadWriteBooleanCharacteristic;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.ReadWriteIntegerCharacteristic;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.ReadWriteTextCharacteristic;

public class AnimationBoilerplate extends Fragment {

    // Could be turned into a subclass
    Handler handler = new Handler(Looper.getMainLooper());

    LayoutInflater myInflater;

    DevKitBtInterface dkInterface = null;

    AnimationServiceInterface animServiceCallback = new AnimationServiceInterface() {
        @Override
        public void onGattServiceFound(BluetoothGattService service) {
            super.onGattServiceFound(service);

            Log.i("AnimationBoilerplate", "onGattServiceFound");

            for (BluetoothGattCharacteristic characteristic : service.getCharacteristics()) {
                Log.i("AnimationBoilerplate", "Got unknown char " + characteristic.getUuid().toString());

                // Lets figure out more about this characteristic
                for (BluetoothGattDescriptor d : characteristic.getDescriptors()) {
                    Log.i("AnimationBoilerplate", "Reading descriptor: " + d.getUuid().toString());
                    dkInterface.queueReadDescriptor(d);
                }
            }
        }
    };

    Map<BluetoothGattCharacteristic, BluetoothGattCharacteristicInfo> chrcToDescMap = new ConcurrentHashMap<>();

    List<DeviceGeneratedUiBase> myDevGeneratedUiElems = new ArrayList<>();

    BluetoothGattCallback callback = new BluetoothGattCallback() {
        @Override
        public void onDescriptorRead(@NonNull BluetoothGatt gatt, @NonNull BluetoothGattDescriptor descriptor, int status, @NonNull byte[] value) {
            super.onDescriptorRead(gatt, descriptor, status, value);

            Log.i("AnimationBoilerplate", "onDescriptorRead: " + descriptor.getUuid().toString() + " status = " +  status);

            if (status == BluetoothStatusCodes.SUCCESS) {
                if (!chrcToDescMap.containsKey(descriptor.getCharacteristic())) {
                    // We have not seen this characteristic yet, insert an empty arraylist into the map
                    chrcToDescMap.put(descriptor.getCharacteristic(), new BluetoothGattCharacteristicInfo());
                    Log.i("AnimationBoilerplate", "Creating new map list");
                }

                BluetoothDescriptorInfo info = new BluetoothDescriptorInfo();
                info.descriptor = descriptor;
                info.value = value;

                // Add descriptor to the list
                Log.i("AnimationBoilerplate", "Adding " + descriptor.getUuid().toString());
                chrcToDescMap.get(descriptor.getCharacteristic()).infos.add(info);
            }

            Log.i("AnimationBoilerplate", "List Len: " + chrcToDescMap.get(descriptor.getCharacteristic()).infos.size());

            if (chrcToDescMap.get(descriptor.getCharacteristic()).addedToUi) {
                return;
            }

            BluetoothDescriptorInfo haveCpf = null;
            BluetoothDescriptorInfo haveCud = null;
            BluetoothDescriptorInfo haveCcc = null;

            for (BluetoothDescriptorInfo i : chrcToDescMap.get(descriptor.getCharacteristic()).infos) {
                Log.i("AnimationBoilerplate", "Testing descriptor " + i.descriptor.getUuid().toString());
                if (i.descriptor.getUuid().equals(getUuidForCpfDescriptor())) {
                    Log.i("AnimationBoilerplate", "Have CPF!");
                    haveCpf = i;
                } else if (i.descriptor.getUuid().equals(getUuidForCudDescriptor())) {
                    Log.i("AnimationBoilerplate", "Have CUD!");
                    haveCud = i;
                } else if (i.descriptor.getUuid().equals(getUuidForCccDescriptor())) {
                    Log.i("AnimationBoilerplate", "Have CCC!");
                    haveCcc = i;
                }
            }

            if (haveCpf != null && haveCud != null && haveCcc != null) {
                Log.i("AnimationBoilerplate",  "Format: " + haveCpf.value[0]);
                String cud = new String(haveCud.value, StandardCharsets.UTF_8);
                Log.i("AnimationBoilerplate",  "Description: " + cud);

                chrcToDescMap.get(descriptor.getCharacteristic()).addedToUi = true;

                byte cpf = haveCpf.value[0];
                if (cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT8 ||
                        cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT16 ||
                        cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UINT32 ||
                        cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_SINT8 ||
                        cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_SINT16 ||
                        cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_SINT32) {
                    // Create a new ReadWriteIntegerCharacteristic and add it to the display
                    BluetoothDescriptorInfo finalHaveCcc2 = haveCcc;
                    handler.post(new Runnable() {
                        @Override
                        public void run() {
                            ReadWriteIntegerCharacteristic rwIntChrc = new ReadWriteIntegerCharacteristic(
                                    myInflater,
                                    myLayout,
                                    descriptor.getCharacteristic(),
                                    cud,
                                    cpf,
                                    finalHaveCcc2.descriptor,
                                    dkInterface);
                            myDevGeneratedUiElems.add(rwIntChrc);
                        }
                    });
                } else if (cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_BOOLEAN) {
                    BluetoothDescriptorInfo finalHaveCcc1 = haveCcc;
                    handler.post(new Runnable() {
                        @Override
                        public void run() {
                            ReadWriteBooleanCharacteristic rwBoolChrc = new ReadWriteBooleanCharacteristic(
                                    myInflater,
                                    myLayout,
                                    descriptor.getCharacteristic(),
                                    cud,
                                    cpf,
                                    finalHaveCcc1.descriptor,
                                    dkInterface);
                            myDevGeneratedUiElems.add(rwBoolChrc);
                        }
                    });
                } else if (cpf == BluetoothHelpers.BLE_GATT_CPF_FORMAT_UTF8S) {
                    BluetoothDescriptorInfo finalHaveCcc = haveCcc;
                    handler.post(new Runnable() {
                        @Override
                        public void run() {
                            ReadWriteTextCharacteristic rwStrChrc = new ReadWriteTextCharacteristic(
                                    myInflater,
                                    myLayout,
                                    descriptor.getCharacteristic(),
                                    cud,
                                    cpf,
                                    finalHaveCcc.descriptor,
                                    dkInterface);
                            myDevGeneratedUiElems.add(rwStrChrc);
                        }
                    });
                }
            }
        }
    };

    LinearLayout myLayout;

    BtServiceType myBtServiceType;

    public void onAnimCreateView(@NonNull LayoutInflater inflater, LinearLayout layout, BtServiceType a) {
        myInflater = inflater;
        myBtServiceType = a;
        myLayout = layout;

        Log.i("AnimationBoilerplate", "onAnimCreateView");

        dkInterface = new DevKitBtInterface(getContext(), animServiceCallback, myBtServiceType);
        dkInterface.registerForGattCallback(callback);
    }
}
