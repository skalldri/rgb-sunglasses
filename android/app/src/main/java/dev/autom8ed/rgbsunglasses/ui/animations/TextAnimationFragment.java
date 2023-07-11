package dev.autom8ed.rgbsunglasses.ui.animations;

import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattService;
import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.Switch;

import androidx.annotation.NonNull;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;


import dev.autom8ed.rgbsunglasses.databinding.FragmentTextanimationBinding;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.DevKitBtInterface;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.IsActiveCharacteristic;
import dev.autom8ed.rgbsunglasses.ui.bluetooth.ReadWriteTextCharacteristic;

public class TextAnimationFragment extends Fragment {

    static final public AnimationType kAnimationType = AnimationType.Text;

    private FragmentTextanimationBinding binding;

    DevKitBtInterface btInterface = null;

    // Number of GATT String slots we expect
    static final public long kNumSlots = 20;
    BluetoothGattCharacteristic[] stringSlot = new BluetoothGattCharacteristic[(int) kNumSlots];

    IsActiveCharacteristic isActiveCharacteristic = null;

    Switch isActiveSw = null;

    EditText[] slotEditText = new EditText[(int) kNumSlots];
    Button[] slotSubmitButton = new Button[(int) kNumSlots];

    List<ReadWriteTextCharacteristic> slotCharacteristics = new ArrayList<>();

    public static UUID getUuidForStringSlotCharacteristic(long slot) {
        UUID uuid = UUID.fromString(String.format("12345678-1234-5678-%04X-56789abc%04X", kAnimationType.ordinal(), slot+1));
        return uuid;
    }

    public int isStringSlotCharacteristic(UUID uuid) {
        for (long i = 0; i < kNumSlots; i++) {
            if (uuid.equals(getUuidForStringSlotCharacteristic(i))) {
                return (int) i;
            }
        }

        return -1;
    }

    AnimationServiceInterface animServiceCallback = new AnimationServiceInterface() {
        @Override
        public void onGattServiceFound(BluetoothGattService service) {
            super.onGattServiceFound(service);

            for (BluetoothGattCharacteristic characteristic : service.getCharacteristics()) {

                if(IsActiveCharacteristic.isIsActiveCharacteristic(characteristic.getUuid(), kAnimationType.ordinal())) {
                    Log.i("TextAnimFrag", "Found IsActive characteristic!");
                    isActiveCharacteristic = new IsActiveCharacteristic(characteristic, isActiveSw, btInterface);
                    continue;
                }
                int slotId = isStringSlotCharacteristic(characteristic.getUuid());
                if (slotId != -1) {
                    Log.i("TextAnimFrag", String.format("Found String Slot %d", slotId));
                    stringSlot[slotId] = characteristic;

                    slotCharacteristics.add(new ReadWriteTextCharacteristic(slotEditText[slotId], slotSubmitButton[slotId], stringSlot[slotId], btInterface));
                }
            }
        }
    };

    public View onCreateView(@NonNull LayoutInflater inflater,
                             ViewGroup container, Bundle savedInstanceState) {
        TextAnimationViewModel textAnimationViewModel =
                new ViewModelProvider(this).get(TextAnimationViewModel.class);

        binding = FragmentTextanimationBinding.inflate(inflater, container, false);
        View root = binding.getRoot();

        slotEditText[0] = binding.editSlot0;
        slotSubmitButton[0] = binding.submitSlot0;

        slotEditText[1] = binding.editSlot1;
        slotSubmitButton[1] = binding.submitSlot1;

        slotEditText[2] = binding.editSlot2;
        slotSubmitButton[2] = binding.submitSlot2;

        slotEditText[3] = binding.editSlot3;
        slotSubmitButton[3] = binding.submitSlot3;

        slotEditText[4] = binding.editSlot4;
        slotSubmitButton[4] = binding.submitSlot4;

        slotEditText[5] = binding.editSlot5;
        slotSubmitButton[5] = binding.submitSlot5;

        slotEditText[6] = binding.editSlot6;
        slotSubmitButton[6] = binding.submitSlot6;

        slotEditText[7] = binding.editSlot7;
        slotSubmitButton[7] = binding.submitSlot7;

        slotEditText[8] = binding.editSlot8;
        slotSubmitButton[8] = binding.submitSlot8;

        slotEditText[9] = binding.editSlot9;
        slotSubmitButton[9] = binding.submitSlot9;

        slotEditText[10] = binding.editSlot10;
        slotSubmitButton[10] = binding.submitSlot10;

        slotEditText[11] = binding.editSlot11;
        slotSubmitButton[11] = binding.submitSlot11;

        slotEditText[12] = binding.editSlot12;
        slotSubmitButton[12] = binding.submitSlot12;

        slotEditText[13] = binding.editSlot13;
        slotSubmitButton[13] = binding.submitSlot13;

        slotEditText[14] = binding.editSlot14;
        slotSubmitButton[14] = binding.submitSlot14;

        slotEditText[15] = binding.editSlot15;
        slotSubmitButton[15] = binding.submitSlot15;

        slotEditText[16] = binding.editSlot16;
        slotSubmitButton[16] = binding.submitSlot16;

        slotEditText[17] = binding.editSlot17;
        slotSubmitButton[17] = binding.submitSlot17;

        slotEditText[18] = binding.editSlot18;
        slotSubmitButton[18] = binding.submitSlot18;

        slotEditText[19] = binding.editSlot19;
        slotSubmitButton[19] = binding.submitSlot19;

        isActiveSw = binding.isTextActiveSwitch;

        // textAnimationViewModel.getSlot0().observe(getViewLifecycleOwner(), slot0::setText);

        Log.i("TextAnimFrag", "onCreateView");

        // Create the BT interface, which will start scanning for the service we want
        btInterface = new DevKitBtInterface(getContext(), animServiceCallback, kAnimationType);

        return root;
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        binding = null;

        Log.i("TextAnimFrag", "onDestroyView");
    }
}
